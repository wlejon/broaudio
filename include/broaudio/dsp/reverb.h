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
    // Original Freeverb gain constants (Jezar at Dreampoint):
    // FIXED_GAIN scales the input before the comb filters to prevent
    // feedback-driven energy buildup. SCALE_WET compensates on the output
    // so the wet signal is roughly unity gain relative to the dry input.
    static constexpr float FIXED_GAIN = 0.015f;
    static constexpr float SCALE_WET = 3.0f;

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
