#pragma once

#include <cstdint>

namespace broaudio {

// Biquad with optional coefficient smoothing.
//
// Usage (snapped — no smoothing, suitable when params change rarely):
//     filter.computeCoefficients(sr);
//     filter.snapToTarget();
//     y = filter.process(x);
//
// Usage (smoothed across a block — suitable for per-block modulation):
//     filter.computeCoefficients(sr);     // writes target coeffs
//     filter.prepareSmoothing(numFrames); // sets per-sample deltas
//     for (i...) {
//         y = filter.process(x);
//         filter.stepSmoothing();         // advance live coeffs once per frame
//     }
//
// process() does not advance smoothing — call stepSmoothing() exactly once
// per output frame. If you don't call prepareSmoothing(), deltas remain zero
// and stepSmoothing() is a no-op, so callers that don't smooth can ignore it.
struct BiquadFilter {
    enum class Type : uint8_t {
        Lowpass, Highpass, Bandpass, Notch,
        Allpass, Peaking, Lowshelf, Highshelf
    };

    Type type = Type::Lowpass;
    float frequency = 1000.0f;
    float Q = 1.0f;
    float gainDB = 0.0f;

    // Live coefficients used by process()
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;

    // Target coefficients written by computeCoefficients()
    float b0t = 1.0f, b1t = 0.0f, b2t = 0.0f;
    float a1t = 0.0f, a2t = 0.0f;

    // Per-sample coefficient deltas (zero = snapped)
    float b0d = 0.0f, b1d = 0.0f, b2d = 0.0f;
    float a1d = 0.0f, a2d = 0.0f;

    float z1[2] = {0.0f, 0.0f};
    float z2[2] = {0.0f, 0.0f};

    bool enabled = false;
    bool primed = false; // true once live coeffs hold a valid filter

    // Compute target coefficients from frequency/Q/type/gainDB.
    void computeCoefficients(int sampleRate);

    // Snap live coefficients to target and clear deltas.
    inline void snapToTarget() {
        b0 = b0t; b1 = b1t; b2 = b2t;
        a1 = a1t; a2 = a2t;
        b0d = b1d = b2d = a1d = a2d = 0.0f;
        primed = true;
    }

    // Set per-sample deltas so live coefficients reach target after numSteps
    // process() calls. If filter is not yet primed, snaps instead.
    inline void prepareSmoothing(int numSteps) {
        if (!primed || numSteps <= 1) {
            snapToTarget();
            return;
        }
        const float inv = 1.0f / static_cast<float>(numSteps);
        b0d = (b0t - b0) * inv;
        b1d = (b1t - b1) * inv;
        b2d = (b2t - b2) * inv;
        a1d = (a1t - a1) * inv;
        a2d = (a2t - a2) * inv;
    }

    inline float process(float input, int ch = 0) {
        float output = b0 * input + z1[ch];
        z1[ch] = b1 * input - a1 * output + z2[ch];
        z2[ch] = b2 * input - a2 * output;
        return output;
    }

    inline void stepSmoothing() {
        b0 += b0d; b1 += b1d; b2 += b2d;
        a1 += a1d; a2 += a2d;
    }

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }
};

} // namespace broaudio
