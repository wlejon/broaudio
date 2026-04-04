#pragma once

#include "broaudio/types.h"
#include <cmath>

namespace broaudio {

// Band-limited waveform generation using polyBLEP anti-aliasing.
float generateSample(Waveform wf, float phase, float phaseInc);

// Equal-power stereo pan: derive L/R gains from pan value (-1..1).
inline void panGains(float pan, float& gainL, float& gainR) {
    float p = (pan + 1.0f) * 0.5f;
    gainL = std::cos(p * 1.5707963f);
    gainR = std::sin(p * 1.5707963f);
}

} // namespace broaudio
