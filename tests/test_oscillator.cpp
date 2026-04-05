#include "test_harness.h"
#include "broaudio/synth/oscillator.h"
#include <cmath>
#include <numbers>
#include <vector>

using namespace broaudio;

// --- Sine ---

TEST(sine_at_zero_phase) {
    float s = generateSample(Waveform::Sine, 0.0f, 0.01f);
    ASSERT_NEAR(s, 0.0f, 1e-5f);
    PASS();
}

TEST(sine_at_quarter_phase) {
    float s = generateSample(Waveform::Sine, 0.25f, 0.01f);
    ASSERT_NEAR(s, 1.0f, 1e-5f);
    PASS();
}

TEST(sine_at_half_phase) {
    float s = generateSample(Waveform::Sine, 0.5f, 0.01f);
    ASSERT_NEAR(s, 0.0f, 1e-5f);
    PASS();
}

TEST(sine_at_three_quarter_phase) {
    float s = generateSample(Waveform::Sine, 0.75f, 0.01f);
    ASSERT_NEAR(s, -1.0f, 1e-5f);
    PASS();
}

// --- Square ---

TEST(square_first_half_positive) {
    // Well away from transitions where polyBLEP acts
    float s = generateSample(Waveform::Square, 0.25f, 0.001f);
    ASSERT_NEAR(s, 1.0f, 0.01f);
    PASS();
}

TEST(square_second_half_negative) {
    float s = generateSample(Waveform::Square, 0.75f, 0.001f);
    ASSERT_NEAR(s, -1.0f, 0.01f);
    PASS();
}

// --- Sawtooth ---

TEST(saw_midpoint) {
    // At phase=0.5, naive saw = 2*0.5-1 = 0. polyBLEP is negligible away from edges.
    float s = generateSample(Waveform::Sawtooth, 0.5f, 0.001f);
    ASSERT_NEAR(s, 0.0f, 0.01f);
    PASS();
}

TEST(saw_range) {
    // Generate a full cycle and check it spans roughly -1 to 1
    float minV = 1.0f, maxV = -1.0f;
    for (int i = 0; i < 1000; i++) {
        float phase = static_cast<float>(i) / 1000.0f;
        float s = generateSample(Waveform::Sawtooth, phase, 0.001f);
        if (s < minV) minV = s;
        if (s > maxV) maxV = s;
    }
    ASSERT_LT(minV, -0.9f);
    ASSERT_GT(maxV, 0.9f);
    PASS();
}

// --- Triangle ---

TEST(triangle_rising_first_half) {
    // Triangle: (phase < 0.5) ? 4*phase-1 : 3-4*phase
    // At phase=0.1, value should be 4*0.1-1 = -0.6
    // At phase=0.4, value should be 4*0.4-1 = 0.6
    float lo = generateSample(Waveform::Triangle, 0.1f, 0.001f);
    float hi = generateSample(Waveform::Triangle, 0.4f, 0.001f);
    ASSERT_GT(hi, lo);
    ASSERT_NEAR(lo, -0.6f, 0.1f);
    ASSERT_NEAR(hi, 0.6f, 0.1f);
    PASS();
}

TEST(triangle_falling_second_half) {
    // At phase=0.6, value should be 3-4*0.6 = 0.6
    // At phase=0.9, value should be 3-4*0.9 = -0.6
    float hi = generateSample(Waveform::Triangle, 0.6f, 0.001f);
    float lo = generateSample(Waveform::Triangle, 0.9f, 0.001f);
    ASSERT_GT(hi, lo);
    ASSERT_NEAR(hi, 0.6f, 0.1f);
    ASSERT_NEAR(lo, -0.6f, 0.1f);
    PASS();
}

// --- Noise ---

TEST(white_noise_range) {
    NoiseState state{};
    float minV = 1.0f, maxV = -1.0f;
    for (int i = 0; i < 10000; i++) {
        float s = generateNoise(Waveform::WhiteNoise, state);
        if (s < minV) minV = s;
        if (s > maxV) maxV = s;
    }
    ASSERT_LT(minV, -0.5f);
    ASSERT_GT(maxV, 0.5f);
    PASS();
}

TEST(white_noise_mean_near_zero) {
    NoiseState state{};
    double sum = 0.0;
    int n = 100000;
    for (int i = 0; i < n; i++) {
        sum += generateNoise(Waveform::WhiteNoise, state);
    }
    float mean = static_cast<float>(sum / n);
    ASSERT_NEAR(mean, 0.0f, 0.05f);
    PASS();
}

TEST(pink_noise_bounded) {
    NoiseState state{};
    for (int i = 0; i < 10000; i++) {
        float s = generateNoise(Waveform::PinkNoise, state);
        ASSERT_TRUE(s >= -2.0f && s <= 2.0f);
    }
    PASS();
}

TEST(brown_noise_bounded) {
    NoiseState state{};
    for (int i = 0; i < 100000; i++) {
        float s = generateNoise(Waveform::BrownNoise, state);
        ASSERT_TRUE(s >= -1.0f && s <= 1.0f);
    }
    PASS();
}

// --- Pan gains ---

TEST(pan_center_equal) {
    float L, R;
    panGains(0.0f, L, R);
    ASSERT_NEAR(L, R, 0.001f);
    // Equal power: each should be ~0.707
    ASSERT_NEAR(L, 0.707f, 0.01f);
    PASS();
}

TEST(pan_hard_left) {
    float L, R;
    panGains(-1.0f, L, R);
    ASSERT_NEAR(L, 1.0f, 0.001f);
    ASSERT_NEAR(R, 0.0f, 0.001f);
    PASS();
}

TEST(pan_hard_right) {
    float L, R;
    panGains(1.0f, L, R);
    ASSERT_NEAR(L, 0.0f, 0.001f);
    ASSERT_NEAR(R, 1.0f, 0.001f);
    PASS();
}

int main() { return runAllTests(); }
