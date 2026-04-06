#include "test_harness.h"
#include "broaudio/dsp/resampler.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace broaudio;

// Generate a mono sine at a given sample rate.
static std::vector<float> makeSine(int sampleRate, float freq, int frames) {
    std::vector<float> buf(frames);
    for (int i = 0; i < frames; i++)
        buf[i] = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / sampleRate);
    return buf;
}

// Measure the dominant frequency in a signal via zero-crossing count.
static float estimateFrequency(const float* data, int frames, int sampleRate) {
    int crossings = 0;
    for (int i = 1; i < frames; i++) {
        if ((data[i - 1] < 0.0f && data[i] >= 0.0f) ||
            (data[i - 1] >= 0.0f && data[i] < 0.0f))
            crossings++;
    }
    // Each full cycle has 2 zero crossings
    float duration = static_cast<float>(frames) / sampleRate;
    return static_cast<float>(crossings) / (2.0f * duration);
}

TEST(identity_resample) {
    // Same rate → output should equal input
    auto input = makeSine(44100, 440.0f, 4410);
    auto output = resample(input.data(), 4410, 1, 44100, 44100);

    ASSERT_EQ(static_cast<int>(output.size()), 4410);
    for (int i = 0; i < 4410; i++) {
        ASSERT_NEAR(output[i], input[i], 1e-6f);
    }
    PASS();
}

TEST(downsample_48k_to_44k) {
    // 48000 → 44100: output should have fewer frames
    int inRate = 48000;
    int outRate = 44100;
    int inFrames = inRate; // 1 second
    auto input = makeSine(inRate, 440.0f, inFrames);

    auto output = resample(input.data(), inFrames, 1, inRate, outRate);

    int expectedFrames = static_cast<int>(std::ceil(
        static_cast<double>(inFrames) * outRate / inRate));
    ASSERT_EQ(static_cast<int>(output.size()), expectedFrames);

    // The resampled signal should still be ~440 Hz
    float freq = estimateFrequency(output.data(), static_cast<int>(output.size()), outRate);
    ASSERT_NEAR(freq, 440.0f, 5.0f); // within 5 Hz

    PASS();
}

TEST(upsample_22k_to_44k) {
    // 22050 → 44100: output should have roughly 2x frames
    int inRate = 22050;
    int outRate = 44100;
    int inFrames = inRate; // 1 second
    auto input = makeSine(inRate, 440.0f, inFrames);

    auto output = resample(input.data(), inFrames, 1, inRate, outRate);

    int expectedFrames = static_cast<int>(std::ceil(
        static_cast<double>(inFrames) * outRate / inRate));
    ASSERT_EQ(static_cast<int>(output.size()), expectedFrames);

    // Frequency should be preserved
    float freq = estimateFrequency(output.data(), static_cast<int>(output.size()), outRate);
    ASSERT_NEAR(freq, 440.0f, 5.0f);

    PASS();
}

TEST(resample_stereo) {
    // Stereo: left = 440 Hz, right = 880 Hz
    int inRate = 48000;
    int outRate = 44100;
    int inFrames = inRate / 2; // 0.5 seconds
    std::vector<float> stereo(inFrames * 2);
    for (int i = 0; i < inFrames; i++) {
        stereo[i * 2]     = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / inRate);
        stereo[i * 2 + 1] = std::sin(2.0f * std::numbers::pi_v<float> * 880.0f * i / inRate);
    }

    auto output = resample(stereo.data(), inFrames, 2, inRate, outRate);

    int expectedFrames = static_cast<int>(std::ceil(
        static_cast<double>(inFrames) * outRate / inRate));
    ASSERT_EQ(static_cast<int>(output.size()), expectedFrames * 2);

    // Deinterleave and check frequencies
    std::vector<float> left(expectedFrames), right(expectedFrames);
    for (int i = 0; i < expectedFrames; i++) {
        left[i] = output[i * 2];
        right[i] = output[i * 2 + 1];
    }

    float freqL = estimateFrequency(left.data(), expectedFrames, outRate);
    float freqR = estimateFrequency(right.data(), expectedFrames, outRate);
    ASSERT_NEAR(freqL, 440.0f, 5.0f);
    ASSERT_NEAR(freqR, 880.0f, 10.0f);

    PASS();
}

TEST(resample_preserves_amplitude) {
    // A 200 Hz sine resampled should maintain similar peak amplitude
    int inRate = 48000;
    int outRate = 44100;
    int inFrames = inRate;
    auto input = makeSine(inRate, 200.0f, inFrames);

    auto output = resample(input.data(), inFrames, 1, inRate, outRate);

    float peak = 0.0f;
    // Skip first and last 1000 samples to avoid edge effects
    for (int i = 1000; i < static_cast<int>(output.size()) - 1000; i++)
        peak = std::max(peak, std::fabs(output[i]));

    // Peak should be close to 1.0 (within filter rolloff tolerance)
    ASSERT_GT(peak, 0.9f);
    ASSERT_LT(peak, 1.05f);

    PASS();
}

TEST(resample_invalid_input) {
    auto result = resample(nullptr, 100, 1, 44100, 48000);
    ASSERT_TRUE(result.empty());

    float dummy = 1.0f;
    result = resample(&dummy, 0, 1, 44100, 48000);
    ASSERT_TRUE(result.empty());

    result = resample(&dummy, 1, 1, 0, 48000);
    ASSERT_TRUE(result.empty());

    PASS();
}

TEST(large_ratio_resample) {
    // 8000 → 44100 (5.5x upsample) — should still work
    int inRate = 8000;
    int outRate = 44100;
    int inFrames = inRate; // 1 second
    auto input = makeSine(inRate, 200.0f, inFrames);

    auto output = resample(input.data(), inFrames, 1, inRate, outRate);

    int expectedFrames = static_cast<int>(std::ceil(
        static_cast<double>(inFrames) * outRate / inRate));
    ASSERT_EQ(static_cast<int>(output.size()), expectedFrames);

    float freq = estimateFrequency(output.data(), static_cast<int>(output.size()), outRate);
    ASSERT_NEAR(freq, 200.0f, 5.0f);

    PASS();
}

int main() { return runAllTests(); }
