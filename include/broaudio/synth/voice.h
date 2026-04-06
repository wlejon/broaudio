#pragma once

#include "broaudio/types.h"
#include "broaudio/dsp/biquad.h"
#include "broaudio/spatial/listener.h"
#include "broaudio/synth/modulation.h"
#include "broaudio/synth/oscillator.h"
#include <atomic>
#include <memory>

namespace broaudio {

class WavetableBank;

// A single oscillator voice. Parameters set from the main thread are atomic;
// envelope/phase state is only touched by the audio thread.
struct Voice {
    int id = 0;

    // Parameters (main thread writes, audio thread reads)
    std::atomic<Waveform> waveform{Waveform::Sine};
    std::atomic<float> frequency{440.0f};
    std::atomic<float> gain{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<float> pitchBend{0.0f};  // semitones offset (applied to frequency)
    std::atomic<int> busId{0};           // target mix bus (0 = master)
    std::atomic<int> sendBusId{-1};      // aux send target (-1 = none)
    std::atomic<float> sendAmount{0.0f}; // aux send level (0-1)
    std::atomic<float> attackRate{0.0f};
    std::atomic<float> decayCoeff{0.0f};
    std::atomic<float> sustainLevel{1.0f};
    std::atomic<float> releaseCoeff{0.0f};

    // Per-voice filter parameters (main thread writes, audio thread reads)
    std::atomic<bool> filterEnabled{false};
    std::atomic<int> filterType{static_cast<int>(BiquadFilter::Type::Lowpass)};
    std::atomic<float> filterFrequency{1000.0f};  // base cutoff Hz
    std::atomic<float> filterQ{1.0f};
    std::atomic<uint32_t> filterVersion{0};

    // Start/stop triggers (main thread writes, audio thread reads & clears)
    std::atomic<bool> triggerStart{false};
    std::atomic<bool> triggerRelease{false};
    std::atomic<double> startTime{-1.0};

    // When true, the audio callback will not auto-purge this voice after
    // its envelope finishes.  Used by VoiceAllocator to keep pooled voices
    // alive for reuse.
    std::atomic<bool> persistent{false};

    // Wavetable (set from main thread, read from audio thread)
    // Shared ownership: multiple voices can reference the same bank.
    std::atomic<std::shared_ptr<const WavetableBank>> wavetable{nullptr};

    // Unison parameters (main thread writes, audio thread reads)
    static constexpr int MAX_UNISON = 8;
    std::atomic<int>   unisonCount{1};          // 1 = off, 2-8 = unison voices
    std::atomic<float> unisonDetune{0.15f};     // total detune spread in semitones
    std::atomic<float> unisonStereoWidth{0.7f}; // 0 = mono center, 1 = full L/R spread
    std::atomic<uint32_t> unisonVersion{0};

    // Audio thread state (only touched by callback)
    float phases[MAX_UNISON] = {};
    bool active = false;
    bool started = false;
    EnvStage envStage = EnvStage::Idle;
    float envLevel = 0.0f;

    // Audio-thread-only unison cache
    int unisonCountCached = 1;
    float unisonDetunes[MAX_UNISON] = {};   // semitone offsets per sub-osc
    float unisonPans[MAX_UNISON] = {};      // pan offsets per sub-osc
    uint32_t unisonVersionSeen = 0;

    // Per-voice modulation state (LFO phases, velocity, note context)
    ModState modState;

    // Per-voice noise state (pink/brown noise accumulators)
    NoiseState noiseState;

    // Per-voice filter state (audio thread only)
    BiquadFilter filter;
    uint32_t filterVersionSeen = 0;

    // Spatial source and directional filter (audio-thread only)
    SpatialSource spatial;
    SpatialFilter spatialFilter;
};

} // namespace broaudio
