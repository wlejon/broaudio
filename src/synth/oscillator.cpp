#include "broaudio/synth/oscillator.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace broaudio {

static inline float polyBLEP(float phase, float phaseInc)
{
    float dt = phaseInc;
    if (dt <= 0.0f) return 0.0f;

    if (phase < dt) {
        float t = phase / dt;
        return t + t - t * t - 1.0f;
    }
    if (phase > 1.0f - dt) {
        float t = (phase - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

float generateSample(Waveform wf, float phase, float phaseInc)
{
    switch (wf) {
        case Waveform::Sine:
            return std::sin(phase * 2.0f * static_cast<float>(M_PI));

        case Waveform::Square: {
            float sample = (phase < 0.5f) ? 1.0f : -1.0f;
            sample += polyBLEP(phase, phaseInc);
            sample -= polyBLEP(std::fmod(phase + 0.5f, 1.0f), phaseInc);
            return sample;
        }

        case Waveform::Sawtooth: {
            float sample = 2.0f * phase - 1.0f;
            sample -= polyBLEP(phase, phaseInc);
            return sample;
        }

        case Waveform::Triangle: {
            float sample = (phase < 0.5f)
                ? (4.0f * phase - 1.0f)
                : (3.0f - 4.0f * phase);
            return sample;
        }
    }
    return 0.0f;
}

} // namespace broaudio
