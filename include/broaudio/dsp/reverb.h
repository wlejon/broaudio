#pragma once

#include <vector>

namespace broaudio {

// Freeverb-style algorithmic stereo reverb.
// 8 parallel comb filters → 4 series allpass filters, per channel.
struct Reverb {
    bool enabled = false;
    float roomSize = 0.85f;   // 0-1, feedback amount in comb filters
    float damping = 0.5f;     // 0-1, lowpass in comb feedback
    float mix = 0.3f;         // wet/dry mix

    void init(int sampleRate);
    void processStereo(float* buf, int numFrames);
    void clear();

private:
    static constexpr int NUM_COMBS = 8;
    static constexpr int NUM_ALLPASS = 4;
    // Normalize the summed comb filter output. Original Freeverb uses
    // scalewet = 3/NUM_COMBS; we use a slightly higher value tuned to
    // keep the wet signal at roughly unity gain relative to the input.
    static constexpr float SCALE_WET = 3.0f / NUM_COMBS;

    struct CombFilter {
        std::vector<float> buffer;
        int pos = 0;
        float filterStore = 0.0f;
    };

    struct AllpassFilter {
        std::vector<float> buffer;
        int pos = 0;
    };

    CombFilter combL_[NUM_COMBS];
    CombFilter combR_[NUM_COMBS];
    AllpassFilter allpassL_[NUM_ALLPASS];
    AllpassFilter allpassR_[NUM_ALLPASS];

    float processComb(CombFilter& comb, float input, float feedback, float damp);
    float processAllpass(AllpassFilter& ap, float input);
};

} // namespace broaudio
