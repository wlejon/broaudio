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

// ---------------------------------------------------------------------------
// Noise generation
// ---------------------------------------------------------------------------

static float whiteNoise()
{
    // Fast xorshift-based white noise
    static uint32_t seed = 1;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
}

float generateNoise(Waveform wf, NoiseState& state)
{
    switch (wf) {
        case Waveform::WhiteNoise:
            return whiteNoise();

        case Waveform::PinkNoise: {
            // Voss-McCartney: update one octave band per sample based on
            // trailing zeros of the index counter
            float white = whiteNoise();
            int idx = state.pinkIndex++;
            // Find lowest set bit to determine which row to update
            for (int i = 0; i < 7; i++) {
                if ((idx & (1 << i)) != 0) {
                    state.pinkRunningSum -= state.pinkRows[i];
                    state.pinkRows[i] = whiteNoise() * 0.25f;
                    state.pinkRunningSum += state.pinkRows[i];
                    break;
                }
            }
            return (state.pinkRunningSum + white * 0.25f) * 0.5f;
        }

        case Waveform::BrownNoise: {
            float white = whiteNoise();
            state.brownValue += white * 0.02f;
            // Clamp to prevent drift
            if (state.brownValue > 1.0f) state.brownValue = 1.0f;
            if (state.brownValue < -1.0f) state.brownValue = -1.0f;
            return state.brownValue;
        }

        default:
            return 0.0f;
    }
}

} // namespace broaudio
