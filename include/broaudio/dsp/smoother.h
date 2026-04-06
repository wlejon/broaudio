#pragma once

#include <cmath>

namespace broaudio {

// One-pole parameter smoother for click-free audio parameter changes.
// Audio-thread only — lives on Voice, Bus, ClipPlayback, or Engine.
//
// Usage:
//   smoother.init(sampleRate);           // once, at setup
//   smoother.set(newTarget);             // each block, from atomic param
//   for (int i = 0; i < N; i++)
//       float val = smoother.next();     // per sample in the inner loop
struct Smoother {
    float value = 0.0f;
    float coeff = 0.014f;  // default ~5ms at 44100 Hz

    // Compute coefficient for a given sample rate and smoothing time.
    // Reaches ~95% of target in timeMs milliseconds.
    void init(int sampleRate, float timeMs = 5.0f) {
        float samples = timeMs * 0.001f * static_cast<float>(sampleRate);
        coeff = (samples > 0.0f) ? 1.0f - std::pow(0.05f, 1.0f / samples) : 1.0f;
    }

    // Set the target value. On first call, snaps to avoid initial ramp from zero.
    void set(float target) {
        if (!initialized_) { value = target; initialized_ = true; }
        target_ = target;
    }

    // Advance one sample toward the target.
    float next() {
        value += coeff * (target_ - value);
        return value;
    }

    // Snap immediately to a value (no smoothing). Use on voice start / retrigger.
    void snap(float val) {
        value = target_ = val;
        initialized_ = true;
    }

private:
    float target_ = 0.0f;
    bool initialized_ = false;
};

} // namespace broaudio
