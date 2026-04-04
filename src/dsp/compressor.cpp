#include "broaudio/dsp/compressor.h"
#include <cmath>

namespace broaudio {

void Compressor::init(int sampleRate)
{
    float sr = static_cast<float>(sampleRate);
    attackCoeff = 1.0f - std::exp(-1.0f / (0.001f * sr));
    releaseCoeff = 1.0f - std::exp(-1.0f / (0.100f * sr));
    envelope = 0.0f;
}

void Compressor::process(float* buffer, int numSamples)
{
    for (int i = 0; i < numSamples; i++) {
        float absLevel = std::fabs(buffer[i]);

        if (absLevel > envelope)
            envelope += attackCoeff * (absLevel - envelope);
        else
            envelope += releaseCoeff * (absLevel - envelope);

        float gain = 1.0f;
        if (envelope > threshold) {
            float target = threshold + (envelope - threshold) / ratio;
            gain = target / envelope;
        }

        buffer[i] *= gain;
    }
}

} // namespace broaudio
