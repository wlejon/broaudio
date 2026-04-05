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

void Compressor::processStereo(float* buffer, int numFrames)
{
    for (int i = 0; i < numFrames; i++) {
        float absL = std::fabs(buffer[i * 2]);
        float absR = std::fabs(buffer[i * 2 + 1]);
        float absLevel = absL > absR ? absL : absR;

        if (absLevel > envelope)
            envelope += attackCoeff * (absLevel - envelope);
        else
            envelope += releaseCoeff * (absLevel - envelope);

        float gain = 1.0f;
        if (envelope > threshold) {
            float target = threshold + (envelope - threshold) / ratio;
            gain = target / envelope;
        }

        buffer[i * 2] *= gain;
        buffer[i * 2 + 1] *= gain;
    }
}

void Compressor::processStereoWithSidechain(float* buffer, const float* sidechain, int numFrames)
{
    for (int i = 0; i < numFrames; i++) {
        // Detect level from the sidechain signal instead of the input
        float absL = std::fabs(sidechain[i * 2]);
        float absR = std::fabs(sidechain[i * 2 + 1]);
        float absLevel = absL > absR ? absL : absR;

        if (absLevel > envelope)
            envelope += attackCoeff * (absLevel - envelope);
        else
            envelope += releaseCoeff * (absLevel - envelope);

        float gain = 1.0f;
        if (envelope > threshold) {
            float target = threshold + (envelope - threshold) / ratio;
            gain = target / envelope;
        }

        buffer[i * 2] *= gain;
        buffer[i * 2 + 1] *= gain;
    }
}

} // namespace broaudio
