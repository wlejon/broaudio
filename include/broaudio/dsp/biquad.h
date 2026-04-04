#pragma once

#include <cstdint>

namespace broaudio {

struct BiquadFilter {
    enum class Type : uint8_t {
        Lowpass, Highpass, Bandpass, Notch,
        Allpass, Peaking, Lowshelf, Highshelf
    };

    Type type = Type::Lowpass;
    float frequency = 1000.0f;
    float Q = 1.0f;
    float gainDB = 0.0f;

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1[2] = {0.0f, 0.0f};
    float z2[2] = {0.0f, 0.0f};

    bool enabled = false;

    void computeCoefficients(int sampleRate);

    inline float process(float input, int ch = 0) {
        float output = b0 * input + z1[ch];
        z1[ch] = b1 * input - a1 * output + z2[ch];
        z2[ch] = b2 * input - a2 * output;
        return output;
    }

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0f; }
};

} // namespace broaudio
