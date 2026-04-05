#pragma once

#include <vector>

namespace broaudio {

// Stereo chorus/flanger effect using modulated delay lines.
// Short delay + LFO modulation = chorus. Very short delay + feedback = flanger.
struct Chorus {
    bool enabled = false;
    float rate = 0.5f;          // LFO rate in Hz
    float depth = 0.005f;       // modulation depth in seconds (5ms default)
    float mix = 0.5f;           // wet/dry mix
    float feedback = 0.0f;      // 0 = chorus, >0 = flanger
    float baseDelay = 0.01f;    // center delay in seconds (10ms default)

    void init(int sampleRate);
    void processStereo(float* buf, int numFrames);

private:
    std::vector<float> bufferL_;
    std::vector<float> bufferR_;
    int writePos_ = 0;
    float phaseL_ = 0.0f;
    float phaseR_ = 0.25f;     // offset for stereo width
    int sampleRate_ = 44100;
    int bufferSize_ = 0;
};

} // namespace broaudio
