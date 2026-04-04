#pragma once

#include <vector>

namespace broaudio {

struct DelayEffect {
    std::vector<float> buffer;
    int writePos = 0;
    int delaySamples = 0;
    float feedback = 0.3f;
    float mix = 0.5f;
    bool enabled = false;

    void init(int maxDelaySamples);
    void processStereo(float* buf, int numFrames);
};

} // namespace broaudio
