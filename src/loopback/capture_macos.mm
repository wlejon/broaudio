// macOS CoreAudio loopback / process-loopback capture.
//
// SystemOutput: a "global" process tap (all processes, excluding none) — the
// system render mix, the macOS analog of WASAPI loopback / a PulseAudio monitor.
//
// ProcessInclude / ProcessExclude: a process tap that mixes down only the named
// process tree (include) or every process except it (exclude). This is the
// direct counterpart to WASAPI's VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK with
// PROCESS_LOOPBACK_MODE_INCLUDE/EXCLUDE — CoreAudio process taps, new in macOS
// 14.2 (AudioHardwareCreateProcessTap + CATapDescription). With muteBehavior
// left at CATapUnmuted the tapped audio still reaches the speakers, so capturing
// does not change what the user hears (the same property the Pulse monitor and
// WASAPI loopback have). No third-party virtual device (BlackHole etc.) needed.
//
// A tap is not itself readable; it is plumbed through a private aggregate device
// whose only sub-node is the tap, and we run a HAL IOProc on that aggregate. The
// tap delivers FP32 at the render device's native rate; we downmix to mono and
// (optionally) resample with SDL exactly like the WASAPI path.
//
// Requires macOS 14.2+. On older systems (or an SDK without the tapping header)
// LoopbackCapture reports unsupported and start() fails — the same graceful
// degradation the other backends use, so callers just hide the feature.
//
// Permission: capturing render audio is TCC-gated. The consumer app must ship an
// NSAudioCaptureUsageDescription string in its Info.plist; the first start()
// triggers the system consent prompt (and blocks on the main thread until the
// user answers, like any TCC dialog). An unsandboxed binary with no Info.plist /
// no grant — e.g. a bare CLI test — has no way to obtain consent, so start() can
// stall at IO bring-up: capture must run from a real, consenting app bundle.
//
// Threading: the IOProc runs on a CoreAudio realtime thread — that is the
// "capture thread" here, so the consumer callback runs off the main thread and
// must do realtime-safe work only. start()/stop()/enumerateProcesses() are
// main-thread only; stop() stops the device + destroys the IOProc (after which
// the callback is guaranteed not to run again) then tears the tap down.

#include "broaudio/loopback_capture.h"
#include "broaudio/log.h"

#include <SDL3/SDL.h>

#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>

// Process-tap API (macOS 14.2+ SDK). Header is ObjC-only and wrapped in
// __OBJC__, so it is safe to include from this .mm.
#if __has_include(<CoreAudio/AudioHardwareTapping.h>)
#  import <CoreAudio/AudioHardwareTapping.h>
#  import <CoreAudio/CATapDescription.h>
#  define BROAUDIO_HAS_AUDIO_TAPS 1
#else
#  define BROAUDIO_HAS_AUDIO_TAPS 0
#endif

#include <libproc.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace broaudio {
namespace {

constexpr AudioObjectPropertyAddress kAddr(AudioObjectPropertySelector sel) {
    return { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
}

// True only when both the SDK has the tapping API and we are running on a
// version that implements it. Everything else degrades to "unsupported".
bool tapsAvailable() {
#if BROAUDIO_HAS_AUDIO_TAPS
    if (@available(macOS 14.2, *)) return true;
#endif
    return false;
}

// Executable basename for a pid (e.g. "Google Chrome"), the macOS analog of the
// WASAPI backend's exe-name display string. Empty if the process is gone.
std::string processName(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return {};
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') base = p + 1;
    return std::string(base);
}

#if BROAUDIO_HAS_AUDIO_TAPS
// Resolve a pid to its CoreAudio process object, the handle CATapDescription
// takes. kAudioObjectUnknown if the process has no audio object (not playing).
AudioObjectID processObjectForPID(pid_t pid) API_AVAILABLE(macos(14.2)) {
    AudioObjectID obj  = kAudioObjectUnknown;
    UInt32        size = sizeof(obj);
    auto addr = kAddr(kAudioHardwarePropertyTranslatePIDToProcessObject);
    OSStatus s = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                            sizeof(pid), &pid, &size, &obj);
    return (s == noErr) ? obj : kAudioObjectUnknown;
}
#endif

} // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct LoopbackCapture::Impl {
    AudioObjectID       tapID  = kAudioObjectUnknown;  // the process tap
    AudioObjectID       aggID  = kAudioObjectUnknown;  // private aggregate wrapping it
    AudioDeviceIOProcID procID = nullptr;              // our IOProc on the aggregate

    LoopbackCallback cb;
    LoopbackConfig   cfg;

    std::atomic<bool> running{false};

    // Source geometry, resolved from the tap's stream format at start().
    int  srcRate        = 0;
    int  srcCh          = 0;
    bool nonInterleaved = false;   // tap may hand us planar (per-channel) buffers

    SDL_AudioStream* resamp = nullptr;   // only when targetRate != srcRate

    // Delivered geometry (read by sampleRate()/channels()/stats()).
    std::atomic<int> deliveredRate{0};
    std::atomic<int> deliveredChannels{0};

    // Stats (written on the IO thread, read on main).
    std::atomic<std::uint64_t> framesDelivered{0};
    std::atomic<std::uint64_t> samplesDelivered{0};
    std::atomic<int>           rollingPeakX10000{0};

    // Scratch reused on the IO thread (sized once in start()).
    std::vector<float> mix;      // downmixed / interleaved float at srcRate
    std::vector<float> pulled;   // resampler output at outRate

    void deliver(const float* s, int n);
    void teardown();

    // HAL IOProc (CoreAudio realtime thread). `ud` is the owning Impl.
    static OSStatus ioProc(AudioObjectID, const AudioTimeStamp*,
                           const AudioBufferList* inInput, const AudioTimeStamp*,
                           AudioBufferList*, const AudioTimeStamp*, void* ud);
};

void LoopbackCapture::Impl::deliver(const float* s, int n) {
    if (n <= 0) return;
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        float a = std::fabs(s[i]);
        if (a > peak) peak = a;
    }
    int pk   = static_cast<int>(peak * 10000.0f);
    int prev = rollingPeakX10000.load(std::memory_order_relaxed);
    while (pk > prev && !rollingPeakX10000.compare_exchange_weak(prev, pk)) {}
    framesDelivered.fetch_add(1, std::memory_order_relaxed);
    samplesDelivered.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
    cb(s, n);
}

OSStatus LoopbackCapture::Impl::ioProc(AudioObjectID, const AudioTimeStamp*,
                                       const AudioBufferList* in, const AudioTimeStamp*,
                                       AudioBufferList*, const AudioTimeStamp*, void* ud) {
    auto* d = static_cast<Impl*>(ud);
    if (!d->running.load(std::memory_order_acquire)) return noErr;
    if (!in || in->mNumberBuffers == 0)             return noErr;

    const int srcCh = d->srcCh;
    if (srcCh <= 0) return noErr;

    // Frames in this cycle, and whether the producer handed us silence.
    int  frames = 0;
    bool silent = (in->mBuffers[0].mData == nullptr);
    if (d->nonInterleaved)
        frames = static_cast<int>(in->mBuffers[0].mDataByteSize / sizeof(float));
    else
        frames = static_cast<int>(in->mBuffers[0].mDataByteSize /
                                  (sizeof(float) * static_cast<std::size_t>(srcCh)));
    if (frames <= 0) return noErr;

    const bool mono  = d->cfg.mono;
    const int  outCh = mono ? 1 : srcCh;
    d->mix.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(outCh));

    if (silent) {
        std::fill(d->mix.begin(), d->mix.end(), 0.0f);
    } else if (d->nonInterleaved) {
        // Planar: one buffer per channel, each `frames` floats.
        if (mono) {
            for (int f = 0; f < frames; ++f) {
                float acc = 0.0f;
                for (int c = 0; c < srcCh; ++c)
                    acc += static_cast<const float*>(in->mBuffers[c].mData)[f];
                d->mix[f] = acc / static_cast<float>(srcCh);
            }
        } else {
            for (int c = 0; c < srcCh; ++c) {
                const auto* src = static_cast<const float*>(in->mBuffers[c].mData);
                for (int f = 0; f < frames; ++f)
                    d->mix[static_cast<std::size_t>(f) * srcCh + c] = src[f];
            }
        }
    } else {
        // Interleaved: a single buffer of frames*srcCh floats.
        const auto* src = static_cast<const float*>(in->mBuffers[0].mData);
        if (mono) {
            for (int f = 0; f < frames; ++f) {
                float acc = 0.0f;
                for (int c = 0; c < srcCh; ++c)
                    acc += src[static_cast<std::size_t>(f) * srcCh + c];
                d->mix[f] = acc / static_cast<float>(srcCh);
            }
        } else {
            std::memcpy(d->mix.data(), src,
                        static_cast<std::size_t>(frames) * srcCh * sizeof(float));
        }
    }

    if (d->resamp) {
        SDL_PutAudioStreamData(d->resamp, d->mix.data(),
                               static_cast<int>(d->mix.size() * sizeof(float)));
        int avail = SDL_GetAudioStreamAvailable(d->resamp);
        if (avail > 0) {
            d->pulled.resize(static_cast<std::size_t>(avail) / sizeof(float));
            int got = SDL_GetAudioStreamData(d->resamp, d->pulled.data(), avail);
            if (got > 0) d->deliver(d->pulled.data(), got / static_cast<int>(sizeof(float)));
        }
    } else if (!d->mix.empty()) {
        d->deliver(d->mix.data(), static_cast<int>(d->mix.size()));
    }
    return noErr;
}

void LoopbackCapture::Impl::teardown() {
    if (aggID != kAudioObjectUnknown && procID) {
        AudioDeviceStop(aggID, procID);
        AudioDeviceDestroyIOProcID(aggID, procID);
        procID = nullptr;
    }
    if (aggID != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(aggID);
        aggID = kAudioObjectUnknown;
    }
#if BROAUDIO_HAS_AUDIO_TAPS
    if (tapID != kAudioObjectUnknown) {
        if (@available(macOS 14.2, *)) AudioHardwareDestroyProcessTap(tapID);
        tapID = kAudioObjectUnknown;
    }
#endif
    if (resamp) {
        SDL_DestroyAudioStream(resamp);
        resamp = nullptr;
    }
}

// ─── public surface ──────────────────────────────────────────────────────────

LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}
LoopbackCapture::~LoopbackCapture() { stop(); }

bool LoopbackCapture::isSupported() { return tapsAvailable(); }

std::vector<AudioProcess> LoopbackCapture::enumerateProcesses() {
    std::vector<AudioProcess> out;
    if (!tapsAvailable()) return out;

    @autoreleasepool {
        auto listAddr = kAddr(kAudioHardwarePropertyProcessObjectList);
        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddr,
                                           0, nullptr, &dataSize) != noErr || dataSize == 0)
            return out;

        std::vector<AudioObjectID> objs(dataSize / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddr, 0, nullptr,
                                       &dataSize, objs.data()) != noErr)
            return out;

        auto runAddr = kAddr(kAudioProcessPropertyIsRunningOutput);
        auto pidAddr = kAddr(kAudioProcessPropertyPID);
        for (AudioObjectID obj : objs) {
            // Only processes actually producing render audio — the candidate set
            // for a picker (matches the WASAPI session enumerator's intent).
            UInt32 running = 0, sz = sizeof(running);
            if (AudioObjectGetPropertyData(obj, &runAddr, 0, nullptr, &sz, &running) != noErr
                || running == 0)
                continue;

            pid_t pid = 0; sz = sizeof(pid);
            if (AudioObjectGetPropertyData(obj, &pidAddr, 0, nullptr, &sz, &pid) != noErr
                || pid <= 0)
                continue;

            bool seen = false;
            for (const auto& p : out) if (p.pid == static_cast<std::uint32_t>(pid)) { seen = true; break; }
            if (seen) continue;

            AudioProcess ap;
            ap.pid  = static_cast<std::uint32_t>(pid);
            ap.name = processName(pid);
            out.push_back(std::move(ap));
        }
    }
    return out;
}

bool LoopbackCapture::start(const LoopbackConfig& cfg, LoopbackCallback cb) {
    if (!cb) return false;
    if (impl_->running.load(std::memory_order_acquire)) return false;
    if (!tapsAvailable()) return false;

#if BROAUDIO_HAS_AUDIO_TAPS
  if (@available(macOS 14.2, *)) {
    impl_->cfg = cfg;
    impl_->cb  = std::move(cb);
    impl_->framesDelivered.store(0, std::memory_order_relaxed);
    impl_->samplesDelivered.store(0, std::memory_order_relaxed);
    impl_->rollingPeakX10000.store(0, std::memory_order_relaxed);

    auto fail = [&](const char* why) {
        log(LogLevel::Error, "broaudio loopback: %s", why);
        impl_->teardown();
        return false;
    };

    @autoreleasepool {
        // ── build the tap description for the requested mode ──────────────────
        CATapDescription* desc = nil;
        if (cfg.mode == LoopbackMode::SystemOutput) {
            // Global tap excluding nobody == the whole system render mix.
            desc = cfg.mono
                ? [[CATapDescription alloc] initMonoGlobalTapButExcludeProcesses:@[]]
                : [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
        } else {
            AudioObjectID po = processObjectForPID(static_cast<pid_t>(cfg.pid));
            if (po == kAudioObjectUnknown)
                return fail("target process has no audio object (not playing?)");
            NSArray* procs = @[ @(po) ];
            if (cfg.mode == LoopbackMode::ProcessInclude)
                desc = cfg.mono ? [[CATapDescription alloc] initMonoMixdownOfProcesses:procs]
                                : [[CATapDescription alloc] initStereoMixdownOfProcesses:procs];
            else  // ProcessExclude
                desc = cfg.mono ? [[CATapDescription alloc] initMonoGlobalTapButExcludeProcesses:procs]
                                : [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:procs];
        }
        if (!desc) return fail("CATapDescription init failed");
        desc.name         = @"broaudio loopback";
        [desc setPrivate:YES];                 // visible only to this process
        desc.muteBehavior = CATapUnmuted;      // keep audio audible while tapping

        // ── create the tap ────────────────────────────────────────────────────
        OSStatus s = AudioHardwareCreateProcessTap(desc, &impl_->tapID);
        NSUUID*  tapUUID = desc.UUID;          // keep before releasing desc
        NSString* aggUID = [NSString stringWithFormat:@"broaudio-loopback-%@",
                                                      tapUUID.UUIDString];
        [desc release];
        if (s != noErr || impl_->tapID == kAudioObjectUnknown)
            return fail("AudioHardwareCreateProcessTap failed");

        // ── resolve the tap's delivered format ────────────────────────────────
        AudioStreamBasicDescription asbd{};
        UInt32 sz = sizeof(asbd);
        auto fmtAddr = kAddr(kAudioTapPropertyFormat);
        if (AudioObjectGetPropertyData(impl_->tapID, &fmtAddr, 0, nullptr, &sz, &asbd) != noErr)
            return fail("tap format query failed");
        if (!(asbd.mFormatFlags & kAudioFormatFlagIsFloat))
            return fail("tap format is not float");
        impl_->srcRate        = static_cast<int>(asbd.mSampleRate);
        impl_->srcCh          = static_cast<int>(asbd.mChannelsPerFrame);
        impl_->nonInterleaved = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
        if (impl_->srcRate <= 0 || impl_->srcCh <= 0)
            return fail("tap reported invalid geometry");

        // ── tap UID, to name it as the aggregate's sole sub-node ──────────────
        CFStringRef tapUID = nullptr;
        sz = sizeof(tapUID);
        auto uidAddr = kAddr(kAudioTapPropertyUID);
        if (AudioObjectGetPropertyData(impl_->tapID, &uidAddr, 0, nullptr, &sz, &tapUID) != noErr
            || !tapUID)
            return fail("tap UID query failed");

        // ── wrap the tap in a private aggregate device we can run an IOProc on ─
        NSDictionary* aggDesc = @{
            @(kAudioAggregateDeviceNameKey):          @"broaudio loopback",
            @(kAudioAggregateDeviceUIDKey):           aggUID,
            @(kAudioAggregateDeviceIsPrivateKey):     @YES,   // not published / persisted
            @(kAudioAggregateDeviceIsStackedKey):     @NO,
            @(kAudioAggregateDeviceTapAutoStartKey):  @YES,
            @(kAudioAggregateDeviceTapListKey): @[ @{
                @(kAudioSubTapUIDKey): (__bridge NSString*)tapUID,
            } ],
        };
        OSStatus s2 = AudioHardwareCreateAggregateDevice(
            (__bridge CFDictionaryRef)aggDesc, &impl_->aggID);
        CFRelease(tapUID);
        if (s2 != noErr || impl_->aggID == kAudioObjectUnknown)
            return fail("AudioHardwareCreateAggregateDevice failed");

        // ── optional resampler + delivered geometry ───────────────────────────
        const int outCh   = cfg.mono ? 1 : impl_->srcCh;
        const int outRate = (cfg.targetRate > 0) ? cfg.targetRate : impl_->srcRate;
        if (outRate != impl_->srcRate) {
            SDL_AudioSpec src{}, dst{};
            src.format = SDL_AUDIO_F32; src.channels = outCh; src.freq = impl_->srcRate;
            dst.format = SDL_AUDIO_F32; dst.channels = outCh; dst.freq = outRate;
            impl_->resamp = SDL_CreateAudioStream(&src, &dst);
            if (!impl_->resamp) return fail("resampler create failed");
        }
        impl_->mix.reserve(static_cast<std::size_t>(impl_->srcRate / 10) * outCh);  // ~100ms
        impl_->deliveredRate.store(outRate, std::memory_order_relaxed);
        impl_->deliveredChannels.store(outCh, std::memory_order_relaxed);

        // ── register the IOProc and go ────────────────────────────────────────
        if (AudioDeviceCreateIOProcID(impl_->aggID, &Impl::ioProc, impl_.get(),
                                      &impl_->procID) != noErr || !impl_->procID)
            return fail("AudioDeviceCreateIOProcID failed");

        impl_->running.store(true, std::memory_order_release);
        if (AudioDeviceStart(impl_->aggID, impl_->procID) != noErr) {
            impl_->running.store(false, std::memory_order_release);
            return fail("AudioDeviceStart failed");
        }
    }
    return true;
  }
#endif
    return false;
}

void LoopbackCapture::stop() {
    impl_->running.store(false, std::memory_order_release);
    impl_->teardown();
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
