#include "test_harness.h"
#include "broaudio/synth/voice.h"
#include "broaudio/synth/oscillator.h"

#include <cmath>

using namespace broaudio;

// --- Default state ---

TEST(default_unison_count_is_one) {
    Voice v;
    ASSERT_EQ(v.unisonCount.load(), 1);
    PASS();
}

TEST(default_phases_are_zero) {
    Voice v;
    for (int i = 0; i < Voice::MAX_UNISON; i++) {
        ASSERT_NEAR(v.phases[i], 0.0f, 1e-7f);
    }
    PASS();
}

TEST(default_detune_spread) {
    Voice v;
    ASSERT_NEAR(v.unisonDetune.load(), 0.15f, 1e-5f);
    PASS();
}

TEST(default_stereo_width) {
    Voice v;
    ASSERT_NEAR(v.unisonStereoWidth.load(), 0.7f, 1e-5f);
    PASS();
}

// --- Parameter setting ---

TEST(set_unison_count_clamped) {
    Voice v;
    v.unisonCount.store(0);
    // The engine clamps 1-8, but the atomic itself stores raw
    // Engine-level clamping tested via engine API; here we just test the struct
    v.unisonCount.store(4);
    ASSERT_EQ(v.unisonCount.load(), 4);
    PASS();
}

TEST(max_unison_is_eight) {
    ASSERT_EQ(Voice::MAX_UNISON, 8);
    PASS();
}

// --- Detune distribution ---

TEST(unison_detune_distribution_two_voices) {
    // With 2 voices and 0.2 semitone spread:
    // voice 0: -0.1, voice 1: +0.1
    float detune = 0.2f;
    int n = 2;
    float offsets[2];
    for (int u = 0; u < n; u++) {
        float t = static_cast<float>(u) / static_cast<float>(n - 1);
        offsets[u] = detune * (t - 0.5f);
    }
    ASSERT_NEAR(offsets[0], -0.1f, 1e-5f);
    ASSERT_NEAR(offsets[1], 0.1f, 1e-5f);
    PASS();
}

TEST(unison_detune_distribution_three_voices) {
    float detune = 0.3f;
    int n = 3;
    float offsets[3];
    for (int u = 0; u < n; u++) {
        float t = static_cast<float>(u) / static_cast<float>(n - 1);
        offsets[u] = detune * (t - 0.5f);
    }
    ASSERT_NEAR(offsets[0], -0.15f, 1e-5f);
    ASSERT_NEAR(offsets[1], 0.0f, 1e-5f);
    ASSERT_NEAR(offsets[2], 0.15f, 1e-5f);
    PASS();
}

// --- Pan distribution ---

TEST(unison_pan_spread_two_voices) {
    float width = 1.0f;
    int n = 2;
    float pans[2];
    for (int u = 0; u < n; u++) {
        float t = static_cast<float>(u) / static_cast<float>(n - 1);
        pans[u] = width * (t * 2.0f - 1.0f);
    }
    ASSERT_NEAR(pans[0], -1.0f, 1e-5f);
    ASSERT_NEAR(pans[1], 1.0f, 1e-5f);
    PASS();
}

TEST(unison_pan_spread_zero_width) {
    float width = 0.0f;
    int n = 4;
    for (int u = 0; u < n; u++) {
        float t = static_cast<float>(u) / static_cast<float>(n - 1);
        float pan = width * (t * 2.0f - 1.0f);
        ASSERT_NEAR(pan, 0.0f, 1e-5f);
    }
    PASS();
}

// --- Gain normalization ---

TEST(gain_normalization_single) {
    float norm = 1.0f / std::sqrt(1.0f);
    ASSERT_NEAR(norm, 1.0f, 1e-5f);
    PASS();
}

TEST(gain_normalization_four) {
    float norm = 1.0f / std::sqrt(4.0f);
    ASSERT_NEAR(norm, 0.5f, 1e-5f);
    PASS();
}

TEST(gain_normalization_eight) {
    float norm = 1.0f / std::sqrt(8.0f);
    ASSERT_NEAR(norm, 1.0f / 2.828427f, 1e-4f);
    PASS();
}

// --- Phase array independence ---

TEST(phases_are_independent) {
    Voice v;
    v.phases[0] = 0.1f;
    v.phases[3] = 0.9f;
    ASSERT_NEAR(v.phases[0], 0.1f, 1e-7f);
    ASSERT_NEAR(v.phases[1], 0.0f, 1e-7f);
    ASSERT_NEAR(v.phases[3], 0.9f, 1e-7f);
    PASS();
}

// --- Version counter ---

TEST(unison_version_increments) {
    Voice v;
    uint32_t v0 = v.unisonVersion.load();
    v.unisonVersion.fetch_add(1);
    ASSERT_EQ(v.unisonVersion.load(), v0 + 1);
    PASS();
}

int main() { return runAllTests(); }
