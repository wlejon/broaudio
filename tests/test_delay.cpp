#include "test_harness.h"
#include "broaudio/dsp/delay.h"
#include <cmath>
#include <vector>

using namespace broaudio;

TEST(delay_correct_delay_time) {
    DelayEffect d;
    d.init(44100 * 2);  // max 2 seconds
    d.enabled = true;
    d.delaySamples = 100;
    d.feedback = 0.0f;
    d.mix = 1.0f;  // 100% wet

    // Send an impulse at frame 0, then silence
    int frames = 200;
    std::vector<float> buf(frames * 2, 0.0f);
    buf[0] = 1.0f;  // L impulse
    buf[1] = 1.0f;  // R impulse

    d.processStereo(buf.data(), frames);

    // At frame 100, we should see the delayed impulse in the wet output
    float delayedL = buf[100 * 2];
    float delayedR = buf[100 * 2 + 1];
    ASSERT_GT(std::fabs(delayedL), 0.5f);
    ASSERT_GT(std::fabs(delayedR), 0.5f);

    // Frames before delay should be ~0 (no output yet)
    ASSERT_NEAR(buf[50 * 2], 0.0f, 0.01f);
    PASS();
}

TEST(delay_feedback_produces_repeats) {
    DelayEffect d;
    d.init(44100 * 2);
    d.enabled = true;
    d.delaySamples = 50;
    d.feedback = 0.5f;
    d.mix = 1.0f;

    int frames = 200;
    std::vector<float> buf(frames * 2, 0.0f);
    buf[0] = 1.0f;
    buf[1] = 1.0f;

    d.processStereo(buf.data(), frames);

    // First repeat at frame 50
    float first = std::fabs(buf[50 * 2]);
    ASSERT_GT(first, 0.3f);

    // Second repeat at frame 100, should be quieter (feedback * first)
    float second = std::fabs(buf[100 * 2]);
    ASSERT_GT(second, 0.1f);
    ASSERT_LT(second, first);
    PASS();
}

TEST(delay_zero_feedback_single_echo) {
    DelayEffect d;
    d.init(44100 * 2);
    d.enabled = true;
    d.delaySamples = 50;
    d.feedback = 0.0f;
    d.mix = 1.0f;

    int frames = 200;
    std::vector<float> buf(frames * 2, 0.0f);
    buf[0] = 1.0f;
    buf[1] = 1.0f;

    d.processStereo(buf.data(), frames);

    // First echo at 50
    ASSERT_GT(std::fabs(buf[50 * 2]), 0.3f);

    // No second echo at 100
    ASSERT_NEAR(buf[100 * 2], 0.0f, 0.01f);
    PASS();
}

TEST(delay_dry_wet_mix) {
    DelayEffect d;
    d.init(44100 * 2);
    d.enabled = true;
    d.delaySamples = 10;
    d.feedback = 0.0f;
    d.mix = 0.0f;  // fully dry

    // Constant signal
    int frames = 50;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        buf[i * 2] = 0.5f;
        buf[i * 2 + 1] = 0.5f;
    }

    d.processStereo(buf.data(), frames);

    // With mix=0, output should be the dry signal
    ASSERT_NEAR(buf[(frames - 1) * 2], 0.5f, 0.01f);
    PASS();
}

int main() { return runAllTests(); }
