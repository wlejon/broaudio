#include "broaudio/dsp/distortion.h"

#include <algorithm>
#include <cmath>

namespace broaudio {

float Distortion::shapeSample(float input) const
{
    float x = input * drive;

    switch (mode) {
        case DistortionMode::SoftClip:
            // tanh saturation — smooth, musical
            return std::tanh(x);

        case DistortionMode::HardClip:
            return std::clamp(x, -1.0f, 1.0f);

        case DistortionMode::Foldback: {
            // Fold signal back when it exceeds [-1, 1]
            // Repeated folding creates harmonically rich overtones
            float threshold = 1.0f;
            while (x > threshold || x < -threshold) {
                if (x > threshold)
                    x = 2.0f * threshold - x;
                else if (x < -threshold)
                    x = -2.0f * threshold - x;
            }
            return x;
        }

        case DistortionMode::Bitcrush: {
            // Bit depth reduction
            float levels = std::pow(2.0f, crushBits) - 1.0f;
            return std::round(x * levels) / levels;
        }
    }

    return x;
}

void Distortion::processStereo(float* buf, int numFrames)
{
    if (!enabled) return;

    // Sample rate reduction step size (1.0 = no reduction)
    float step = 1.0f / std::max(crushRate, 0.01f);
    bool doCrushRate = (mode == DistortionMode::Bitcrush && crushRate < 0.999f);

    for (int i = 0; i < numFrames; i++) {
        float dryL = buf[i * 2];
        float dryR = buf[i * 2 + 1];

        float wetL, wetR;

        if (doCrushRate) {
            // Sample-and-hold for rate reduction
            holdCounter_ += 1.0f;
            if (holdCounter_ >= step) {
                holdCounter_ -= step;
                holdL_ = shapeSample(dryL);
                holdR_ = shapeSample(dryR);
            }
            wetL = holdL_;
            wetR = holdR_;
        } else {
            wetL = shapeSample(dryL);
            wetR = shapeSample(dryR);
        }

        wetL *= outputGain;
        wetR *= outputGain;

        buf[i * 2]     = dryL + (wetL - dryL) * mix;
        buf[i * 2 + 1] = dryR + (wetR - dryR) * mix;
    }
}

} // namespace broaudio
