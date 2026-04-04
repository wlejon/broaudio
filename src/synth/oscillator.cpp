#include "broaudio/synth/oscillator.h"
#include <cmath>
#include <numbers>

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

// Integrated polyBLEP (polyBLAMP) for smoothing slope discontinuities (e.g. triangle corners).
static inline float polyBLAMP(float phase, float phaseInc)
{
    float dt = phaseInc;
    if (dt <= 0.0f) return 0.0f;

    if (phase < dt) {
        float t = phase / dt;
        // Integral of polyBLEP, scaled by dt
        return dt * (t * t * (t / 3.0f - 0.5f) - t + 0.5f);
    }
    if (phase > 1.0f - dt) {
        float t = (phase - 1.0f) / dt;
        return -dt * (t * t * (t / 3.0f + 0.5f) + t + 0.5f);
    }
    return 0.0f;
}

float generateSample(Waveform wf, float phase, float phaseInc)
{
    switch (wf) {
        case Waveform::Sine:
            return std::sin(phase * 2.0f * std::numbers::pi_v<float>);

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
            // polyBLAMP at the two slope-change corners (phase=0 and phase=0.5).
            // Slope changes by +8 at phase=0 and -8 at phase=0.5.
            sample += 8.0f * polyBLAMP(phase, phaseInc);
            sample -= 8.0f * polyBLAMP(std::fmod(phase + 0.5f, 1.0f), phaseInc);
            return sample;
        }
    }
    return 0.0f;
}

} // namespace broaudio
