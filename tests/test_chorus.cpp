#include "test_harness.h"
#include "broaudio/dsp/chorus.h"
#include <cmath>
#include <vector>
#include <numbers>

using namespace broaudio;

static constexpr int SR = 44100;

TEST(chorus_modulates_signal) {
    Chorus ch;
    ch.init(SR);
    ch.enabled = true;
    ch.rate = 1.0f;
    ch.depth = 0.005f;
    ch.mix = 0.5f;
    ch.feedback = 0.0f;
    ch.baseDelay = 0.01f;

    // Feed a sine wave — chorus should produce amplitude modulation via comb filtering
    int frames = SR;  // 1 second
    std::vector<float> buf(frames * 2);
    float freq = 440.0f;
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    ch.processStereo(buf.data(), frames);

    // Output should differ from input (dry+wet creates variation)
    // Check that the output is not identical to dry sine
    bool differs = false;
    for (int i = frames / 2; i < frames; i++) {
        float dry = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / SR);
        if (std::fabs(buf[i * 2] - dry) > 0.01f) {
            differs = true;
            break;
        }
    }
    ASSERT_TRUE(differs);
    PASS();
}

TEST(chorus_dry_when_mix_zero) {
    Chorus ch;
    ch.init(SR);
    ch.enabled = true;
    ch.rate = 1.0f;
    ch.depth = 0.005f;
    ch.mix = 0.0f;  // fully dry
    ch.feedback = 0.0f;
    ch.baseDelay = 0.01f;

    int frames = 1000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        buf[i * 2] = 0.5f;
        buf[i * 2 + 1] = 0.5f;
    }

    ch.processStereo(buf.data(), frames);

    ASSERT_NEAR(buf[(frames - 1) * 2], 0.5f, 0.01f);
    PASS();
}

TEST(chorus_feedback_creates_flanger) {
    Chorus ch;
    ch.init(SR);
    ch.enabled = true;
    ch.rate = 0.5f;
    ch.depth = 0.002f;
    ch.mix = 0.5f;
    ch.feedback = 0.7f;  // flanger territory
    ch.baseDelay = 0.003f;

    // Feed a sine wave
    int frames = SR;
    std::vector<float> buf(frames * 2);
    float freq = 440.0f;
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    ch.processStereo(buf.data(), frames);

    // Feedback should cause comb filtering — output differs from input
    // Just verify it didn't blow up and produces a bounded signal
    float peak = 0.0f;
    for (int i = 0; i < frames * 2; i++) {
        float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }
    ASSERT_LT(peak, 5.0f);
    ASSERT_GT(peak, 0.1f);
    PASS();
}

TEST(chorus_stereo_has_width) {
    Chorus ch;
    ch.init(SR);
    ch.enabled = true;
    ch.rate = 1.0f;
    ch.depth = 0.005f;
    ch.mix = 1.0f;
    ch.feedback = 0.0f;
    ch.baseDelay = 0.01f;

    // Mono input
    int frames = SR;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    ch.processStereo(buf.data(), frames);

    // L and R should differ due to the phase offset between channels
    bool differs = false;
    for (int i = frames / 2; i < frames; i++) {
        if (std::fabs(buf[i * 2] - buf[i * 2 + 1]) > 0.01f) {
            differs = true;
            break;
        }
    }
    ASSERT_TRUE(differs);
    PASS();
}

int main() { return runAllTests(); }
