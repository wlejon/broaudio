// Tests for Engine::setMicFrameCallback — the audio-thread mic-frame hook.
//
// Verifies that:
//   1. A registered callback receives exactly the samples fed to the mic
//      processing pipeline, with the expected sample count.
//   2. Passing nullptr cleanly disables the hook.
//   3. After mutex removal, the SPSC analysis ring (micBuffer) still
//      delivers the same samples to readLatest().

#include "test_harness.h"
#include "broaudio/engine.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;

static std::vector<float> ramp(int n) {
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) v[i] = static_cast<float>(i) * 0.001f;
    return v;
}

TEST(mic_frame_callback_receives_samples) {
    Engine e; e.initHeadless();

    std::vector<float> captured;
    int callCount = 0;
    e.setMicFrameCallback([&](const float* s, int n) {
        captured.insert(captured.end(), s, s + n);
        callCount++;
    });

    auto in1 = ramp(128);
    auto in2 = ramp(64);
    e.testFeedMicSamples(in1.data(), static_cast<int>(in1.size()));
    e.testFeedMicSamples(in2.data(), static_cast<int>(in2.size()));

    ASSERT_EQ(callCount, 2);
    ASSERT_EQ(static_cast<int>(captured.size()), 128 + 64);
    for (int i = 0; i < 128; i++) ASSERT_NEAR(captured[i], in1[i], 1e-6f);
    for (int i = 0; i < 64; i++)  ASSERT_NEAR(captured[128 + i], in2[i], 1e-6f);
    PASS();
}

TEST(mic_frame_callback_nullptr_disables) {
    Engine e; e.initHeadless();

    int callCount = 0;
    e.setMicFrameCallback([&](const float*, int) { callCount++; });

    auto in = ramp(32);
    e.testFeedMicSamples(in.data(), 32);
    ASSERT_EQ(callCount, 1);

    e.setMicFrameCallback(nullptr);
    e.testFeedMicSamples(in.data(), 32);
    e.testFeedMicSamples(in.data(), 32);
    ASSERT_EQ(callCount, 1);  // unchanged after disable
    PASS();
}

TEST(mic_buffer_still_delivers_after_mutex_removal) {
    Engine e; e.initHeadless();

    // No frame callback installed — exercise the bare ring-write path.
    std::vector<float> in(256);
    for (int i = 0; i < 256; i++) in[i] = std::sin(2.0f * PI * i / 64.0f);

    e.testFeedMicSamples(in.data(), 256);

    std::vector<float> out(256, 0.0f);
    e.micBuffer().readLatest(out.data(), 256);

    // Last 256 samples written must equal the input.
    for (int i = 0; i < 256; i++) ASSERT_NEAR(out[i], in[i], 1e-6f);
    PASS();
}

TEST(mic_frame_callback_replace_swaps_target) {
    Engine e; e.initHeadless();

    int aCalls = 0, bCalls = 0;
    e.setMicFrameCallback([&](const float*, int) { aCalls++; });
    auto in = ramp(16);
    e.testFeedMicSamples(in.data(), 16);
    ASSERT_EQ(aCalls, 1);
    ASSERT_EQ(bCalls, 0);

    e.setMicFrameCallback([&](const float*, int) { bCalls++; });
    e.testFeedMicSamples(in.data(), 16);
    e.testFeedMicSamples(in.data(), 16);
    ASSERT_EQ(aCalls, 1);
    ASSERT_EQ(bCalls, 2);
    PASS();
}

int main() { return runAllTests(); }
