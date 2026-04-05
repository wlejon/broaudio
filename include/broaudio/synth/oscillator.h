#pragma once

#include "broaudio/types.h"
#include <cmath>

namespace broaudio {

// Band-limited waveform generation using polyBLEP anti-aliasing.
float generateSample(Waveform wf, float phase, float phaseInc);

// Noise state for pink and brown noise (per-voice, audio-thread-only).
struct NoiseState {
    // Pink noise: Voss-McCartney algorithm (7 octave bands)
    float pinkRows[7] = {};
    float pinkRunningSum = 0.0f;
    int pinkIndex = 0;

    // Brown noise: integrated white noise
    float brownValue = 0.0f;
};

// Generate one noise sample. Updates state in-place.
float generateNoise(Waveform wf, NoiseState& state);

// Equal-power stereo pan: derive L/R gains from pan value (-1..1).
inline void panGains(float pan, float& gainL, float& gainR) {
    float p = (pan + 1.0f) * 0.5f;
    gainL = std::cos(p * 1.5707963f);
    gainR = std::sin(p * 1.5707963f);
}

} // namespace broaudio
