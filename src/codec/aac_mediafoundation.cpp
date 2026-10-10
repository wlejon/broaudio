// AAC through Media Foundation's AAC decoder MFT (Windows 8+): the decoder
// found with MFTEnumEx, raw access units in (MF_MT_AAC_PAYLOAD_TYPE 0, the
// AudioSpecificConfig after the HEAACWAVEINFO tail in MF_MT_USER_DATA),
// float32 (or 16-bit PCM, converted) out.
//
// COM: the process's multithreaded apartment is kept alive once
// (CoIncrementMTAUsage), so a thread that never initialised COM — the stream
// worker — uses the decoder as an implicit MTA member, and a thread that is
// an STA (a UI thread) keeps its apartment; nothing here calls CoInitialize.

#include "aac_decoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace broaudio {

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) p->Release(); p = nullptr; }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    T* get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string hrText(const char* what, HRESULT hr)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lx)", what, static_cast<unsigned long>(hr));
    return buf;
}

// Once per process: keep the MTA alive and start Media Foundation.
bool startPlatform(std::string* error)
{
    static std::once_flag once;
    static HRESULT result = E_FAIL;
    std::call_once(once, [] {
        using IncrementFn = HRESULT(WINAPI*)(void**);
        if (HMODULE combase = LoadLibraryW(L"combase.dll")) {
            auto inc = reinterpret_cast<IncrementFn>(
                reinterpret_cast<void*>(GetProcAddress(combase, "CoIncrementMTAUsage")));
            void* cookie = nullptr;
            if (inc) inc(&cookie);       // never decremented: the MTA lives as long as the process
        }
        result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    });
    if (FAILED(result) && error) *error = hrText("Media Foundation startup", result);
    return SUCCEEDED(result);
}

class MfAacDecoder final : public AacDecoder {
public:
    ~MfAacDecoder() override
    {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
    }

    bool init(const uint8_t* asc, size_t ascSize, std::string* error)
    {
        AacConfig cfg;
        if (!parseAudioSpecificConfig(asc, ascSize, cfg)) {
            if (error) *error = "corrupt MP4 audio (unreadable AudioSpecificConfig)";
            return false;
        }
        if (!startPlatform(error)) return false;

        MFT_REGISTER_TYPE_INFO in{MFMediaType_Audio, MFAudioFormat_AAC};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
                               MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                               &in, nullptr, &acts, &count);
        if (FAILED(hr) || count == 0) {
            if (acts) CoTaskMemFree(acts);
            if (error) *error = "no AAC decoder is installed (Media Foundation has none; on Windows N "
                                "editions install the Media Feature Pack)";
            return false;
        }
        for (UINT32 i = 0; i < count; ++i) {
            if (!mft_) acts[i]->ActivateObject(IID_PPV_ARGS(mft_.put()));
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
        if (!mft_) {
            if (error) *error = "the AAC decoder would not start";
            return false;
        }

        Com<IMFMediaType> t;
        if (FAILED(hr = MFCreateMediaType(t.put()))) return fail(error, "MFCreateMediaType", hr);
        const int inRate = cfg.extSampleRate ? cfg.extSampleRate : cfg.sampleRate;
        const int inCh = cfg.channels > 0 ? (cfg.channels == 7 ? 8 : std::min(cfg.channels, 6)) : 2;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(inRate));
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(inCh));
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                         // raw access units
        t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);    // not specified
        // HEAACWAVEINFO after its WAVEFORMATEX: payload type, profile/level,
        // struct type, two reserved fields (12 bytes); then the ASC.
        std::vector<uint8_t> user(12, 0);
        user[2] = 0xFE;
        user.insert(user.end(), asc, asc + ascSize);
        t->SetBlob(MF_MT_USER_DATA, user.data(), static_cast<UINT32>(user.size()));
        if (FAILED(hr = mft_->SetInputType(0, t.get(), 0))) return fail(error, "AAC decoder input type", hr);
        if (!pickOutput(error)) return false;

        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool decode(const uint8_t* packet, size_t size, std::vector<float>& out) override
    {
        if (!mft_ || !packet || !size) return false;
        Com<IMFSample> s;
        Com<IMFMediaBuffer> b;
        if (FAILED(MFCreateSample(s.put())) || FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), b.put())))
            return false;
        BYTE* p = nullptr;
        if (FAILED(b->Lock(&p, nullptr, nullptr))) return false;
        std::memcpy(p, packet, size);
        b->Unlock();
        b->SetCurrentLength(static_cast<DWORD>(size));
        s->AddBuffer(b.get());

        HRESULT hr = mft_->ProcessInput(0, s.get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (!drainOutput(out)) return false;
            hr = mft_->ProcessInput(0, s.get(), 0);
        }
        if (FAILED(hr)) return false;
        return drainOutput(out);
    }

    void reset() override
    {
        if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    bool drain(std::vector<float>& out) override
    {
        if (!mft_) return false;
        if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) return false;
        return drainOutput(out);
    }

    // One frame: the decoder's output trails its input by a packet (measured:
    // a 1024-frame AAC-LC packet in gives the previous packet's 1024 out).
    int latency() const override { return firstFrames_ ? firstFrames_ : 1024; }

    int sampleRate() const override { return rate_; }
    int channels() const override { return channels_; }

private:
    bool fail(std::string* error, const char* what, HRESULT hr)
    {
        if (error) *error = hrText(what, hr);
        return false;
    }

    // Float output when the decoder offers it, else 16-bit PCM.
    bool pickOutput(std::string* error)
    {
        Com<IMFMediaType> best;
        int bestScore = -1;
        for (DWORD i = 0;; ++i) {
            Com<IMFMediaType> t;
            HRESULT hr = mft_->GetOutputAvailableType(0, i, t.put());
            if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            UINT32 bits = 0;
            t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            int score = -1;
            if (sub == MFAudioFormat_Float && bits == 32) score = 2;
            else if (sub == MFAudioFormat_PCM && (bits == 16 || bits == 24 || bits == 32)) score = 1;
            if (score > bestScore) {
                bestScore = score;
                best.reset();
                best.p = t.p;
                t.p = nullptr;
            }
        }
        if (!best) {
            if (error) *error = "the AAC decoder offers no PCM output";
            return false;
        }
        HRESULT hr = mft_->SetOutputType(0, best.get(), 0);
        if (FAILED(hr)) return fail(error, "AAC decoder output type", hr);
        GUID sub{};
        best->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 v = 0;
        floatOut_ = sub == MFAudioFormat_Float;
        if (SUCCEEDED(best->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &v))) bits_ = static_cast<int>(v);
        if (SUCCEEDED(best->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v))) rate_ = static_cast<int>(v);
        if (SUCCEEDED(best->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v))) channels_ = static_cast<int>(v);

        MFT_OUTPUT_STREAM_INFO info{};
        if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &info))) {
            providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            outBytes_ = info.cbSize;
        }
        // Room for an HE-AAC frame (2048 frames) of 8 channels of float, whatever the MFT asks.
        outBytes_ = std::max<DWORD>(outBytes_, 2048 * 8 * 4);
        return true;
    }

    // Every output the decoder has ready.
    bool drainOutput(std::vector<float>& out)
    {
        for (int guard = 0; guard < 64; ++guard) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            Com<IMFSample> mine;
            if (!providesSamples_) {
                Com<IMFMediaBuffer> b;
                if (FAILED(MFCreateSample(mine.put())) || FAILED(MFCreateMemoryBuffer(outBytes_, b.put())))
                    return false;
                mine->AddBuffer(b.get());
                ob.pSample = mine.get();
            }
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (providesSamples_ && ob.pSample) ob.pSample->Release();
                return true;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (providesSamples_ && ob.pSample) ob.pSample->Release();
                if (!pickOutput(nullptr)) return false;
                continue;
            }
            if (FAILED(hr)) {
                if (providesSamples_ && ob.pSample) ob.pSample->Release();
                return false;
            }
            if (ob.pSample) append(ob.pSample, out);
            if (providesSamples_ && ob.pSample) ob.pSample->Release();
        }
        return true;
    }

    void append(IMFSample* s, std::vector<float>& out)
    {
        Com<IMFMediaBuffer> b;
        if (FAILED(s->ConvertToContiguousBuffer(b.put()))) return;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(b->Lock(&p, nullptr, &len))) return;
        if (!firstFrames_ && len && channels_ > 0)
            firstFrames_ = static_cast<int>(len / (static_cast<DWORD>(bits_ / 8) * static_cast<DWORD>(channels_)));
        if (floatOut_) {
            const size_t n = len / sizeof(float);
            const size_t at = out.size();
            out.resize(at + n);
            std::memcpy(out.data() + at, p, n * sizeof(float));
        } else if (bits_ == 16) {
            const size_t n = len / 2;
            const int16_t* v = reinterpret_cast<const int16_t*>(p);
            for (size_t i = 0; i < n; ++i) out.push_back(v[i] / 32768.0f);
        } else if (bits_ == 24) {
            const size_t n = len / 3;
            for (size_t i = 0; i < n; ++i) {
                const int32_t x = static_cast<int32_t>((uint32_t(p[3 * i]) << 8) | (uint32_t(p[3 * i + 1]) << 16) |
                                                       (uint32_t(p[3 * i + 2]) << 24)) >> 8;
                out.push_back(x / 8388608.0f);
            }
        } else if (bits_ == 32) {
            const size_t n = len / 4;
            const int32_t* v = reinterpret_cast<const int32_t*>(p);
            for (size_t i = 0; i < n; ++i) out.push_back(static_cast<float>(v[i] / 2147483648.0));
        }
        b->Unlock();
    }

    Com<IMFTransform> mft_;
    bool floatOut_ = true;
    int bits_ = 32;
    int rate_ = 0;
    int channels_ = 0;
    bool providesSamples_ = false;
    DWORD outBytes_ = 0;
    int firstFrames_ = 0;
};

} // namespace

std::unique_ptr<AacDecoder> makePlatformAacDecoder(const uint8_t* asc, size_t ascSize, std::string* error)
{
    auto d = std::make_unique<MfAacDecoder>();
    if (!d->init(asc, ascSize, error)) return nullptr;
    return d;
}

} // namespace broaudio
