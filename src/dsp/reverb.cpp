#include "broaudio/dsp/reverb.h"

#include <cstring>

namespace broaudio {

// Freeverb tuning constants (original Freeverb by Jezar at Dreampoint).
// Comb filter delay lengths in samples at 44100 Hz, scaled for actual sample rate.
static constexpr int COMB_LENGTHS[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static constexpr int ALLPASS_LENGTHS[4] = {556, 441, 341, 225};
static constexpr int STEREO_SPREAD = 23;  // offset for right channel
static constexpr float ALLPASS_FEEDBACK = 0.5f;

void Reverb::init(int sampleRate)
{
    float scale = static_cast<float>(sampleRate) / 44100.0f;

    for (int i = 0; i < NUM_COMBS; i++) {
        int lenL = static_cast<int>(COMB_LENGTHS[i] * scale);
        int lenR = static_cast<int>((COMB_LENGTHS[i] + STEREO_SPREAD) * scale);
        combL_[i].buffer.assign(lenL, 0.0f);
        combL_[i].pos = 0;
        combL_[i].filterStore = 0.0f;
        combR_[i].buffer.assign(lenR, 0.0f);
        combR_[i].pos = 0;
        combR_[i].filterStore = 0.0f;
    }

    for (int i = 0; i < NUM_ALLPASS; i++) {
        int lenL = static_cast<int>(ALLPASS_LENGTHS[i] * scale);
        int lenR = static_cast<int>((ALLPASS_LENGTHS[i] + STEREO_SPREAD) * scale);
        allpassL_[i].buffer.assign(lenL, 0.0f);
        allpassL_[i].pos = 0;
        allpassR_[i].buffer.assign(lenR, 0.0f);
        allpassR_[i].pos = 0;
    }
}

void Reverb::clear()
{
    for (int i = 0; i < NUM_COMBS; i++) {
        std::memset(combL_[i].buffer.data(), 0, combL_[i].buffer.size() * sizeof(float));
        combL_[i].filterStore = 0.0f;
        std::memset(combR_[i].buffer.data(), 0, combR_[i].buffer.size() * sizeof(float));
        combR_[i].filterStore = 0.0f;
    }
    for (int i = 0; i < NUM_ALLPASS; i++) {
        std::memset(allpassL_[i].buffer.data(), 0, allpassL_[i].buffer.size() * sizeof(float));
        std::memset(allpassR_[i].buffer.data(), 0, allpassR_[i].buffer.size() * sizeof(float));
    }
}

float Reverb::processComb(CombFilter& comb, float input, float feedback, float damp)
{
    float output = comb.buffer[comb.pos];
    comb.filterStore = output * (1.0f - damp) + comb.filterStore * damp;
    comb.buffer[comb.pos] = input + comb.filterStore * feedback;
    comb.pos++;
    if (comb.pos >= static_cast<int>(comb.buffer.size())) comb.pos = 0;
    return output;
}

float Reverb::processAllpass(AllpassFilter& ap, float input)
{
    float buffered = ap.buffer[ap.pos];
    float output = -input + buffered;
    ap.buffer[ap.pos] = input + buffered * ALLPASS_FEEDBACK;
    ap.pos++;
    if (ap.pos >= static_cast<int>(ap.buffer.size())) ap.pos = 0;
    return output;
}

void Reverb::processStereo(float* buf, int numFrames)
{
    float feedback = roomSize;
    float damp1 = damping;
    float wet = mix * SCALE_WET;
    float dry = 1.0f - mix;

    for (int i = 0; i < numFrames; i++) {
        float inL = buf[i * 2];
        float inR = buf[i * 2 + 1];
        float mono = (inL + inR) * 0.5f;

        // Sum parallel comb filters
        float outL = 0.0f;
        float outR = 0.0f;
        for (int c = 0; c < NUM_COMBS; c++) {
            outL += processComb(combL_[c], mono, feedback, damp1);
            outR += processComb(combR_[c], mono, feedback, damp1);
        }

        // Series allpass filters
        for (int a = 0; a < NUM_ALLPASS; a++) {
            outL = processAllpass(allpassL_[a], outL);
            outR = processAllpass(allpassR_[a], outR);
        }

        buf[i * 2]     = inL * dry + outL * wet;
        buf[i * 2 + 1] = inR * dry + outR * wet;
    }
}

} // namespace broaudio
