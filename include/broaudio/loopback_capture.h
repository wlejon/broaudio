#pragma once

// System-audio / application loopback capture.
//
// broaudio owns mic capture (one SDL_AudioStream against the default RECORDING
// device). This is the complement: capturing what the machine is PLAYING — the
// render side — so the listening stack (bro.sense / bro.kws / streaming STT)
// can hear the computer itself, not just a microphone.
//
// Three modes (see LoopbackMode):
//   * SystemOutput   — the whole default render endpoint (the system mix).
//   * ProcessInclude — only one application's audio (a target process + the
//                      processes it spawned). "Transcribe just this call."
//   * ProcessExclude — everything EXCEPT that process tree.
//
// Unlike the mic path this does not go through SDL: render-loopback and
// per-process loopback are OS-specific (Windows WASAPI today; other platforms
// report unsupported and never start). The capture runs on its own thread and
// fans mono FP32 frames out through a callback, exactly like a mic tap — so a
// consumer (e.g. bro's shared listen host) can write the same PcmRing it fills
// from the mic. The callback runs on the capture thread: do real-time-safe work
// only (write a ring, no allocation, no locks, no blocking).
//
// Lifecycle (start / stop / enumerateProcesses) is main-thread only. There is
// no lock on the audio path: the capture thread is the sole producer and stop()
// signals it with an atomic + joins.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace broaudio {

enum class LoopbackMode {
    SystemOutput,    // entire default render endpoint (the system mix)
    ProcessInclude,  // only the target process tree's audio
    ProcessExclude,  // everything except the target process tree
};

// An application currently holding an audio render session — the candidate set
// for ProcessInclude/ProcessExclude. `pid` is the process to target; `name` is
// the executable name (e.g. "chrome.exe") for display.
struct AudioProcess {
    std::uint32_t pid = 0;
    std::string   name;
};

// All fields optional except mode (and pid for the Process* modes).
//   * targetRate = 0  -> deliver at the capture device's native rate.
//                        >0 resamples to this rate before the callback.
//   * mono = true     -> downmix to a single channel (what the listen stack
//                        wants). false delivers interleaved channels.
struct LoopbackConfig {
    LoopbackMode  mode       = LoopbackMode::SystemOutput;
    std::uint32_t pid        = 0;     // target for ProcessInclude/ProcessExclude
    int           targetRate = 0;     // 0 = native; else resample to this
    bool          mono       = true;  // downmix to mono
};

struct LoopbackStats {
    bool          running          = false;
    std::uint64_t framesDelivered  = 0;   // callback invocations
    std::uint64_t samplesDelivered = 0;   // total samples handed to the callback
    float         rollingPeak      = 0.0f;
};

// Mono (or interleaved, if cfg.mono==false) FP32 at the delivered rate.
using LoopbackCallback = std::function<void(const float* samples, int numSamples)>;

class LoopbackCapture {
public:
    LoopbackCapture();
    ~LoopbackCapture();

    LoopbackCapture(const LoopbackCapture&)            = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;

    // True if this build/OS can capture render audio at all. When false,
    // start() always fails and enumerateProcesses() returns empty — callers
    // should hide the feature rather than treat it as an error.
    static bool isSupported();

    // Applications with an active render audio session, for a "pick an app"
    // picker. May be empty (nothing playing, or unsupported). Deduplicated by
    // pid. Main thread only.
    static std::vector<AudioProcess> enumerateProcesses();

    // Begin capture. `cb` is invoked on the capture thread with mono FP32 at
    // sampleRate(). Returns false if already running, unsupported, the target
    // process is gone, or the OS refused the device. Main thread only.
    bool start(const LoopbackConfig& cfg, LoopbackCallback cb);

    // Stop and join the capture thread. Safe to call when not running. After
    // this returns the callback is guaranteed not to run again. Main thread.
    void stop();

    bool isCapturing() const;

    // The delivered stream's rate / channel count (valid while capturing;
    // channels() is 1 when cfg.mono was set). 0 when not running.
    int sampleRate() const;
    int channels() const;

    LoopbackStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace broaudio
