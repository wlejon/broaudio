#pragma once

namespace broaudio {

struct Compressor {
    float envelope = 0.0f;
    float threshold = 0.7f;
    float ratio = 4.0f;
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;

    void init(int sampleRate);
    void process(float* buffer, int numSamples);
};

} // namespace broaudio
