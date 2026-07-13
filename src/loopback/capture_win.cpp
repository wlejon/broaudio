// Windows WASAPI loopback / process-loopback capture.
//
// SystemOutput: open the default render endpoint in loopback mode
// (AUDCLNT_STREAMFLAGS_LOOPBACK), poll its capture client, downmix to mono,
// resample, hand to the callback.
//
// ProcessInclude / ProcessExclude: activate the VIRTUAL_AUDIO_DEVICE_PROCESS_
// LOOPBACK device via ActivateAudioInterfaceAsync with AUDIOCLIENT_ACTIVATION_
// PARAMS naming a target pid + include/exclude-tree mode. Requires Windows 10
// build 19041+ (the audioclientactivationparams.h header). When that header is
// absent the process modes report unsupported; SystemOutput still works.
//
// All WASAPI interface use lives on the capture thread (one COM apartment, no
// cross-thread marshaling). start() waits for that thread to report init
// success/failure through an event; stop() sets an atomic and joins.

#include "broaudio/loopback_capture.h"
#include "broaudio/log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>

// Process-loopback activation params (Win10 2004+ SDK).
#if __has_include(<audioclientactivationparams.h>)
#  include <audioclientactivationparams.h>
#  define BROAUDIO_HAS_PROCESS_LOOPBACK 1
#else
#  define BROAUDIO_HAS_PROCESS_LOOPBACK 0
#endif

#include <mmreg.h>
#include <ksmedia.h>

namespace broaudio {
namespace {

// ─── tiny COM helpers ───────────────────────────────────────────────────────

template <typename T>
void safeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string processName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH];
    DWORD len = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        const wchar_t* base = buf;
        for (const wchar_t* p = buf; *p; ++p) {
            if (*p == L'\\' || *p == L'/') base = p + 1;
        }
        name = wideToUtf8(base);
    }
    CloseHandle(h);
    return name;
}

// RAII CoInitialize that tolerates an already-initialized apartment.
struct ComScope {
    bool    inited = false;
    HRESULT hr     = S_OK;
    explicit ComScope(DWORD model) {
        hr = CoInitializeEx(nullptr, model);
        inited = SUCCEEDED(hr);            // RPC_E_CHANGED_MODE -> already up, don't uninit
    }
    ~ComScope() { if (inited) CoUninitialize(); }
};

// ─── format description resolved from the source ────────────────────────────

enum class SampleKind { Float32, Int16, Int32, Unsupported };

SampleKind classifyFormat(const WAVEFORMATEX* wfx) {
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32)
        return SampleKind::Float32;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16)
        return SampleKind::Int16;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 32)
        return SampleKind::Int32;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && wfx->wBitsPerSample == 32)
            return SampleKind::Float32;
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && wfx->wBitsPerSample == 16)
            return SampleKind::Int16;
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && wfx->wBitsPerSample == 32)
            return SampleKind::Int32;
    }
    return SampleKind::Unsupported;
}

#if BROAUDIO_HAS_PROCESS_LOOPBACK
// Completion handler for ActivateAudioInterfaceAsync: just signals an event so
// the capture thread can collect the activation result synchronously.
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~ActivateHandler() { if (done) CloseHandle(done); }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done);
        return S_OK;
    }
    // IUnknown — static lifetime (stack), so refcounting is a no-op.
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            return S_OK;
        }
        // ActivateAudioInterfaceAsync marshals the callback across apartments
        // and refuses (E_ILLEGAL_METHOD_CALL) a non-agile handler. IAgileObject
        // is a marker with no methods beyond IUnknown, so the IUnknown vtable
        // satisfies it.
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IUnknown*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
};
#endif

} // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct LoopbackCapture::Impl {
    std::thread       thread;
    std::atomic<bool> stopFlag{false};
    std::atomic<bool> running{false};

    LoopbackCallback  cb;
    LoopbackConfig    cfg;

    // Resolved at init, read by stats()/sampleRate()/channels() after start().
    std::atomic<int>  deliveredRate{0};
    std::atomic<int>  deliveredChannels{0};

    // Stats (written on capture thread, read on main).
    std::atomic<std::uint64_t> framesDelivered{0};
    std::atomic<std::uint64_t> samplesDelivered{0};
    std::atomic<int>           rollingPeakX10000{0};

    // start() handshake: thread sets startOk then signals startDone.
    std::atomic<bool> startOk{false};
    HANDLE            startDone = nullptr;

    void run();                              // capture-thread entry
    void deliver(const float* s, int n);     // mono/interleaved float -> callback + stats
};

void LoopbackCapture::Impl::deliver(const float* s, int n) {
    if (n <= 0) return;
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        float a = std::fabs(s[i]);
        if (a > peak) peak = a;
    }
    int pk = static_cast<int>(peak * 10000.0f);
    int prev = rollingPeakX10000.load(std::memory_order_relaxed);
    while (pk > prev && !rollingPeakX10000.compare_exchange_weak(prev, pk)) {}
    framesDelivered.fetch_add(1, std::memory_order_relaxed);
    samplesDelivered.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
    cb(s, n);
}

void LoopbackCapture::Impl::run() {
    ComScope com(COINIT_MULTITHREADED);
    (void)com;

    IAudioClient*        client  = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX*        mixFmt  = nullptr;   // owned (CoTaskMemFree) for SystemOutput
    SDL_AudioStream*     resamp  = nullptr;
    HANDLE               evt     = nullptr;
    bool                 useEvent = false;

    int  srcRate = 0, srcCh = 0;
    SampleKind kind = SampleKind::Unsupported;

    HRESULT lastHr = S_OK;
    auto fail = [&](const char* why) {
        log(LogLevel::Error, "broaudio loopback: %s (hr=0x%08lX)", why,
            static_cast<unsigned long>(lastHr));
        startOk.store(false, std::memory_order_release);
        if (startDone) SetEvent(startDone);
    };

    auto cleanup = [&]() {
        if (client) client->Stop();
        if (resamp) SDL_DestroyAudioStream(resamp);
        safeRelease(capture);
        safeRelease(client);
        if (mixFmt) CoTaskMemFree(mixFmt);
        if (evt) CloseHandle(evt);
    };

    // ── device + client setup ────────────────────────────────────────────
    if (cfg.mode == LoopbackMode::SystemOutput) {
        IMMDeviceEnumerator* devEnum = nullptr;
        IMMDevice*           dev     = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&devEnum));
        if (SUCCEEDED(hr))
            hr = devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr))
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&client));
        safeRelease(dev);
        safeRelease(devEnum);
        if (FAILED(hr) || !client) { fail("no default render endpoint"); cleanup(); return; }

        hr = client->GetMixFormat(&mixFmt);
        if (FAILED(hr) || !mixFmt) { fail("GetMixFormat failed"); cleanup(); return; }

        kind = classifyFormat(mixFmt);
        if (kind == SampleKind::Unsupported) { fail("unsupported mix format"); cleanup(); return; }
        srcRate = static_cast<int>(mixFmt->nSamplesPerSec);
        srcCh   = static_cast<int>(mixFmt->nChannels);

        // 200 ms buffer, loopback flag, polled (no event).
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                2000000 /*hns = 200ms*/, 0, mixFmt, nullptr);
        if (FAILED(hr)) { fail("loopback Initialize failed"); cleanup(); return; }
    } else {
#if BROAUDIO_HAS_PROCESS_LOOPBACK
        if (cfg.pid == 0) { fail("process loopback needs a pid"); cleanup(); return; }

        AUDIOCLIENT_ACTIVATION_PARAMS ap{};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = cfg.pid;
        ap.ProcessLoopbackParams.ProcessLoopbackMode =
            (cfg.mode == LoopbackMode::ProcessInclude)
                ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT pv{};
        pv.vt           = VT_BLOB;
        pv.blob.cbSize  = sizeof(ap);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);

        ActivateHandler handler;
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                 __uuidof(IAudioClient), &pv,
                                                 &handler, &op);
        lastHr = hr;
        if (FAILED(hr)) { fail("ActivateAudioInterfaceAsync failed"); cleanup(); return; }
        WaitForSingleObject(handler.done, 3000);

        HRESULT actHr = E_FAIL;
        IUnknown* unk = nullptr;
        if (op) { op->GetActivateResult(&actHr, &unk); op->Release(); }
        lastHr = actHr;
        if (FAILED(actHr) || !unk) { fail("process loopback activation refused"); cleanup(); return; }
        unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
        unk->Release();
        if (!client) { fail("process loopback: no IAudioClient"); cleanup(); return; }

        // The virtual device has no mix format; we declare one. 16-bit PCM
        // stereo @ 48 kHz is the well-supported request (auto-converted).
        WAVEFORMATEX fmt{};
        fmt.wFormatTag      = WAVE_FORMAT_PCM;
        fmt.nChannels       = 2;
        fmt.nSamplesPerSec  = 48000;
        fmt.wBitsPerSample  = 16;
        fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize          = 0;
        kind    = SampleKind::Int16;
        srcRate = 48000;
        srcCh   = 2;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                2000000, 0, &fmt, nullptr);
        lastHr = hr;
        if (FAILED(hr)) { fail("process loopback Initialize failed"); cleanup(); return; }

        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt || FAILED(client->SetEventHandle(evt))) { fail("SetEventHandle failed"); cleanup(); return; }
        useEvent = true;
#else
        fail("process loopback not supported by this SDK");
        cleanup();
        return;
#endif
    }

    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&capture)))) {
        fail("GetService(IAudioCaptureClient) failed"); cleanup(); return;
    }

    // ── resampler (optional) + delivered geometry ────────────────────────
    const int outCh   = cfg.mono ? 1 : srcCh;
    const int outRate = (cfg.targetRate > 0) ? cfg.targetRate : srcRate;
    if (outRate != srcRate) {
        SDL_AudioSpec src{}, dst{};
        src.format = SDL_AUDIO_F32; src.channels = outCh; src.freq = srcRate;
        dst.format = SDL_AUDIO_F32; dst.channels = outCh; dst.freq = outRate;
        resamp = SDL_CreateAudioStream(&src, &dst);
        if (!resamp) { fail("resampler create failed"); cleanup(); return; }
    }
    deliveredRate.store(outRate, std::memory_order_relaxed);
    deliveredChannels.store(outCh, std::memory_order_relaxed);

    if (FAILED(client->Start())) { fail("IAudioClient::Start failed"); cleanup(); return; }

    // init succeeded — release start()
    startOk.store(true, std::memory_order_release);
    if (startDone) SetEvent(startDone);

    // ── capture loop ──────────────────────────────────────────────────────
    std::vector<float> mono;     // downmixed / interleaved float at srcRate
    std::vector<float> pulled;   // resampler output at outRate
    while (!stopFlag.load(std::memory_order_acquire)) {
        if (useEvent) WaitForSingleObject(evt, 100);
        else          Sleep(10);

        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet))) break;
        while (packet > 0 && !stopFlag.load(std::memory_order_acquire)) {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            mono.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(outCh));

            if (silent || !data) {
                std::fill(mono.begin(), mono.end(), 0.0f);
            } else if (cfg.mono) {
                for (UINT32 f = 0; f < frames; ++f) {
                    float acc = 0.0f;
                    for (int c = 0; c < srcCh; ++c) {
                        float v = 0.0f;
                        if (kind == SampleKind::Float32)
                            v = reinterpret_cast<const float*>(data)[f * srcCh + c];
                        else if (kind == SampleKind::Int16)
                            v = reinterpret_cast<const int16_t*>(data)[f * srcCh + c] / 32768.0f;
                        else  // Int32
                            v = reinterpret_cast<const int32_t*>(data)[f * srcCh + c] / 2147483648.0f;
                        acc += v;
                    }
                    mono[f] = acc / static_cast<float>(srcCh);
                }
            } else {
                const std::size_t total = static_cast<std::size_t>(frames) * srcCh;
                for (std::size_t i = 0; i < total; ++i) {
                    if (kind == SampleKind::Float32)
                        mono[i] = reinterpret_cast<const float*>(data)[i];
                    else if (kind == SampleKind::Int16)
                        mono[i] = reinterpret_cast<const int16_t*>(data)[i] / 32768.0f;
                    else
                        mono[i] = reinterpret_cast<const int32_t*>(data)[i] / 2147483648.0f;
                }
            }
            capture->ReleaseBuffer(frames);

            if (resamp) {
                SDL_PutAudioStreamData(resamp, mono.data(),
                                       static_cast<int>(mono.size() * sizeof(float)));
                int avail = SDL_GetAudioStreamAvailable(resamp);
                if (avail > 0) {
                    pulled.resize(static_cast<std::size_t>(avail) / sizeof(float));
                    int got = SDL_GetAudioStreamData(resamp, pulled.data(), avail);
                    if (got > 0) deliver(pulled.data(), got / static_cast<int>(sizeof(float)));
                }
            } else if (!mono.empty()) {
                deliver(mono.data(), static_cast<int>(mono.size()));
            }

            if (FAILED(capture->GetNextPacketSize(&packet))) { packet = 0; break; }
        }
    }

    cleanup();
}

// ─── public surface ───────────────────────────────────────────────────────

LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}
LoopbackCapture::~LoopbackCapture() { stop(); }

bool LoopbackCapture::isSupported() { return true; }

std::vector<AudioProcess> LoopbackCapture::enumerateProcesses() {
    std::vector<AudioProcess> out;
    ComScope com(COINIT_MULTITHREADED);

    IMMDeviceEnumerator*    devEnum  = nullptr;
    IMMDevice*              dev      = nullptr;
    IAudioSessionManager2*  mgr      = nullptr;
    IAudioSessionEnumerator* sessEnum = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&devEnum));
    if (SUCCEEDED(hr)) hr = devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                          nullptr, reinterpret_cast<void**>(&mgr));
    if (SUCCEEDED(hr)) hr = mgr->GetSessionEnumerator(&sessEnum);

    if (SUCCEEDED(hr) && sessEnum) {
        int count = 0;
        sessEnum->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl*  ctrl  = nullptr;
            IAudioSessionControl2* ctrl2 = nullptr;
            if (FAILED(sessEnum->GetSession(i, &ctrl)) || !ctrl) continue;
            if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                               reinterpret_cast<void**>(&ctrl2))) && ctrl2) {
                DWORD pid = 0;
                if (SUCCEEDED(ctrl2->GetProcessId(&pid)) && pid != 0 &&
                    ctrl2->IsSystemSoundsSession() == S_FALSE) {
                    bool seen = false;
                    for (const auto& p : out) if (p.pid == pid) { seen = true; break; }
                    if (!seen) {
                        AudioProcess ap;
                        ap.pid  = pid;
                        ap.name = processName(pid);
                        out.push_back(std::move(ap));
                    }
                }
            }
            safeRelease(ctrl2);
            safeRelease(ctrl);
        }
    }

    safeRelease(sessEnum);
    safeRelease(mgr);
    safeRelease(dev);
    safeRelease(devEnum);
    return out;
}

bool LoopbackCapture::start(const LoopbackConfig& cfg, LoopbackCallback cb) {
    if (!cb) return false;
    if (impl_->running.load(std::memory_order_acquire)) return false;
#if !BROAUDIO_HAS_PROCESS_LOOPBACK
    if (cfg.mode != LoopbackMode::SystemOutput) return false;
#endif

    impl_->cfg       = cfg;
    impl_->cb        = std::move(cb);
    impl_->stopFlag.store(false, std::memory_order_relaxed);
    impl_->startOk.store(false, std::memory_order_relaxed);
    impl_->framesDelivered.store(0, std::memory_order_relaxed);
    impl_->samplesDelivered.store(0, std::memory_order_relaxed);
    impl_->rollingPeakX10000.store(0, std::memory_order_relaxed);
    impl_->startDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    impl_->thread = std::thread([this] { impl_->run(); });

    // Wait for the thread to report init success/failure (device + Start()).
    if (impl_->startDone) WaitForSingleObject(impl_->startDone, 5000);
    const bool ok = impl_->startOk.load(std::memory_order_acquire);
    if (impl_->startDone) { CloseHandle(impl_->startDone); impl_->startDone = nullptr; }

    if (!ok) {
        impl_->stopFlag.store(true, std::memory_order_release);
        if (impl_->thread.joinable()) impl_->thread.join();
        return false;
    }
    impl_->running.store(true, std::memory_order_release);
    return true;
}

void LoopbackCapture::stop() {
    impl_->stopFlag.store(true, std::memory_order_release);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running.store(false, std::memory_order_release);
    impl_->deliveredRate.store(0, std::memory_order_relaxed);
    impl_->deliveredChannels.store(0, std::memory_order_relaxed);
}

bool LoopbackCapture::isCapturing() const {
    return impl_->running.load(std::memory_order_acquire);
}

int LoopbackCapture::sampleRate() const {
    return impl_->deliveredRate.load(std::memory_order_relaxed);
}

int LoopbackCapture::channels() const {
    return impl_->deliveredChannels.load(std::memory_order_relaxed);
}

LoopbackStats LoopbackCapture::stats() const {
    LoopbackStats s;
    s.running          = impl_->running.load(std::memory_order_acquire);
    s.framesDelivered  = impl_->framesDelivered.load(std::memory_order_relaxed);
    s.samplesDelivered = impl_->samplesDelivered.load(std::memory_order_relaxed);
    s.rollingPeak      = impl_->rollingPeakX10000.load(std::memory_order_relaxed) / 10000.0f;
    return s;
}

} // namespace broaudio
