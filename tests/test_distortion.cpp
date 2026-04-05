#include "test_harness.h"
#include "broaudio/dsp/distortion.h"
#include <cmath>
#include <vector>
#include <numbers>

using namespace broaudio;

static constexpr int SR = 44100;

// --- Soft clip ---

TEST(softclip_saturates_hot_signal) {
    Distortion dist;
    dist.enabled = true;
    dist.mode = DistortionMode::SoftClip;
    dist.drive = 10.0f;
    dist.mix = 1.0f;
    dist.outputGain = 1.0f;

    int frames = 1000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    dist.processStereo(buf.data(), frames);

    // tanh(10*x) should be close to +/-1 at peaks, never exceed 1
    float peak = 0.0f;
    for (int i = 0; i < frames * 2; i++) {
        float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }
    ASSERT_LT(peak, 1.001f);
    ASSERT_GT(peak, 0.9f);
    PASS();
}

// --- Hard clip ---

TEST(hardclip_clamps_at_one) {
    Distortion dist;
    dist.enabled = true;
    dist.mode = DistortionMode::HardClip;
    dist.drive = 5.0f;
    dist.mix = 1.0f;
    dist.outputGain = 1.0f;

    int frames = 1000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    dist.processStereo(buf.data(), frames);

    for (int i = 0; i < frames * 2; i++) {
        ASSERT_LT(buf[i], 1.001f);
        ASSERT_GT(buf[i], -1.001f);
    }
    PASS();
}

// --- Foldback ---

TEST(foldback_stays_bounded) {
    Distortion dist;
    dist.enabled = true;
    dist.mode = DistortionMode::Foldback;
    dist.drive = 8.0f;
    dist.mix = 1.0f;
    dist.outputGain = 1.0f;

    int frames = 1000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    dist.processStereo(buf.data(), frames);

    float peak = 0.0f;
    for (int i = 0; i < frames * 2; i++) {
        float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }
    // Foldback should stay within [-1, 1]
    ASSERT_LT(peak, 1.001f);
    PASS();
}

// --- Bitcrush ---

TEST(bitcrush_quantizes_signal) {
    Distortion dist;
    dist.enabled = true;
    dist.mode = DistortionMode::Bitcrush;
    dist.drive = 1.0f;
    dist.mix = 1.0f;
    dist.outputGain = 1.0f;
    dist.crushBits = 4.0f;  // 16 levels — very quantized
    dist.crushRate = 1.0f;

    int frames = 1000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    dist.processStereo(buf.data(), frames);

    // With 4-bit crush (15 levels), output values should be quantized to discrete steps
    // Count unique values — should be far fewer than the 1000 input samples
    std::vector<float> unique;
    for (int i = 0; i < frames; i++) {
        float v = buf[i * 2];
        bool found = false;
        for (float u : unique) {
            if (std::fabs(v - u) < 0.001f) { found = true; break; }
        }
        if (!found) unique.push_back(v);
    }
    // 4 bits = 15 levels, sine goes through ~30 steps round trip
    ASSERT_LT(static_cast<int>(unique.size()), 32);
    PASS();
}

// --- Mix control ---

TEST(mix_zero_passes_dry_signal) {
    Distortion dist;
    dist.enabled = true;
    dist.mode = DistortionMode::SoftClip;
    dist.drive = 10.0f;
    dist.mix = 0.0f;  // fully dry
    dist.outputGain = 1.0f;

    int frames = 1000;
    std::vector<float> buf(frames * 2);
    std::vector<float> dry(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
        dry[i * 2] = s;
        dry[i * 2 + 1] = s;
    }

    dist.processStereo(buf.data(), frames);

    for (int i = 0; i < frames * 2; i++) {
        ASSERT_NEAR(buf[i], dry[i], 0.0001f);
    }
    PASS();
}

// --- Disabled is passthrough ---

TEST(disabled_is_passthrough) {
    Distortion dist;
    dist.enabled = false;
    dist.drive = 50.0f;

    int frames = 100;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        buf[i * 2] = 0.7f;
        buf[i * 2 + 1] = -0.3f;
    }

    dist.processStereo(buf.data(), frames);

    ASSERT_NEAR(buf[0], 0.7f, 0.0001f);
    ASSERT_NEAR(buf[1], -0.3f, 0.0001f);
    PASS();
}

int main() { return runAllTests(); }
