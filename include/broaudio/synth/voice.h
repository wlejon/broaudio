#pragma once

#include "broaudio/types.h"
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
    std::atomic<int> busId{0};           // target mix bus (0 = master)
    std::atomic<float> attackRate{0.0f};
    std::atomic<float> decayCoeff{0.0f};
    std::atomic<float> sustainLevel{1.0f};
    std::atomic<float> releaseCoeff{0.0f};

    // Start/stop triggers (main thread writes, audio thread reads & clears)
    std::atomic<bool> triggerStart{false};
    std::atomic<bool> triggerRelease{false};
    std::atomic<double> startTime{-1.0};

    // Wavetable (set from main thread, read from audio thread)
    // Shared ownership: multiple voices can reference the same bank.
    std::atomic<std::shared_ptr<const WavetableBank>> wavetable{nullptr};

    // Audio thread state (only touched by callback)
    float phase = 0.0f;
    bool active = false;
    bool started = false;
    EnvStage envStage = EnvStage::Idle;
    float envLevel = 0.0f;
};

} // namespace broaudio
