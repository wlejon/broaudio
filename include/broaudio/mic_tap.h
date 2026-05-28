#pragma once

// Multi-consumer mic-frame dispatch with per-consumer resampling and AGC.
//
// broaudio owns mic capture (one SDL_AudioStream against the default recording
// device). Consumers — wake-word detectors, live ASR, custom analyzers — attach
// a MicTap describing the rate, frame size, and loudness normalisation they
// want. The audio thread fans the raw mic chunk out to every active tap: each
// tap resamples (if its targetRate differs from the engine rate), optionally
// AGCs, slices into the requested chunkFrames, and invokes the user callback.
//
// This is the single audio-thread mic hook. It gives us:
//   * Multiple simultaneous consumers (wake + future live whisper, etc.) with
//     no last-writer-wins races.
//   * One place that knows about resampling — taps requesting different rates
//     each get a polyphase SDL_AudioStream owned by broaudio.
//   * AGC as a reusable utility instead of per-consumer copy-paste.
//   * Stats per tap (frames delivered, rolling peak) for diagnostics without
//     instrumenting the consumer.

#include <atomic>
#include <cstdint>
#include <functional>

namespace broaudio {

// Rolling peak-track-and-lift AGC.
//
// On each chunk: find peak, decay the running peak by elapsed time (half-life),
// take max, derive a single scalar gain (clamped at maxGain), apply in-place.
// Chunks below noiseGate decay the running peak but do not raise it — that
// prevents room hiss from being amplified to speech-band loudness during quiet
// stretches.
//
// Default tuning lifts a ~0.1 peak mic toward ~0.95 over ~1 s with a gate that
// ignores anything below 0.01 RMS-ish — matches the training distribution of
// the brosoundml wake model (peak-normalised clips at 0.99).
struct AgcConfig {
    float targetPeak     = 0.95f;
    float halfLifeSec    = 1.0f;
    float noiseGate      = 0.01f;
    float maxGain        = 10.0f;
};

// Single-threaded AGC state. One instance per stream/consumer; mutate from the
// thread that owns the consumer (the audio thread, for taps).
class Agc {
public:
    Agc() = default;
    explicit Agc(AgcConfig cfg) : cfg_(cfg) {}

    void setConfig(AgcConfig cfg) { cfg_ = cfg; }
    const AgcConfig& config() const { return cfg_; }

    void reset() { runningPeak_ = 0.0f; }

    // Apply in-place. `sampleRate` controls how fast the running peak decays
    // relative to chunk length.
    void apply(float* samples, int numSamples, int sampleRate);

    float runningPeak() const { return runningPeak_; }

private:
    AgcConfig cfg_{};
    float     runningPeak_ = 0.0f;
};

// Configuration handed to Engine::addMicTap. All fields optional.
//   * targetRate  = 0  -> deliver at the engine's native mic rate (no
//                          resampling done; the tap callback receives the same
//                          samples the SDL recording callback produced).
//   * chunkFrames = 0  -> deliver each resampler-output run as a single
//                          callback (variable size). >0 buffers and slices into
//                          fixed-size frames.
//   * agc=false       -> deliver samples unchanged. true enables Agc with
//                          agcCfg.
struct MicTapConfig {
    int       targetRate  = 0;
    int       chunkFrames = 0;
    bool      agc         = false;
    AgcConfig agcCfg      = {};
};

using MicTapCallback = std::function<void(const float* samples, int numSamples)>;
using MicTapId       = std::uint32_t;

inline constexpr MicTapId kInvalidMicTapId = 0;

struct MicTapStats {
    std::uint64_t framesDelivered  = 0;   // number of callback invocations
    std::uint64_t samplesDelivered = 0;   // total samples handed to callback
    float         rollingPeak      = 0.0f;
};

} // namespace broaudio
