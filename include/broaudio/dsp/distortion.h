#pragma once

#include "broaudio/dsp/params.h"

namespace broaudio {

// Stereo distortion / waveshaper effect.
// Supports soft clip (tanh), hard clip, foldback, and bitcrush modes.
struct Distortion {
    bool enabled = false;
    DistortionMode mode = DistortionMode::SoftClip;
    float drive = 1.0f;
    float mix = 1.0f;
    float outputGain = 1.0f;
    float crushBits = 16.0f;
    float crushRate = 1.0f;

    void processStereo(float* buf, int numFrames);

private:
    // Bitcrush sample-and-hold state
    float holdL_ = 0.0f;
    float holdR_ = 0.0f;
    float holdCounter_ = 0.0f;

    float shapeSample(float input) const;
};

} // namespace broaudio
