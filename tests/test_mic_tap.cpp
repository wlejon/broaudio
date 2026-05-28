// Tests for Engine::addMicTap / removeMicTap / getMicTapStats — the multi-
// consumer mic-frame dispatch path (the sole audio-thread mic hook).
//
// Covers:
//   * Single tap at the engine rate, no AGC, no chunking — receives every
//     sample fed to the mic pipeline.
//   * Multiple taps observe the same chunk independently.
//   * AGC lifts a quiet signal toward the target peak.
//   * chunkFrames slices the stream into fixed-size deliveries.
//   * removeMicTap stops delivery without affecting other taps.
//   * Stats reflect delivered work.
//
// Samples are pushed via Engine::injectMicSamples (the offline/headless seam),
// which drives the same tap dispatch the recording callback uses. Resampling
// is covered separately — these tests run at the engine rate so they don't
// depend on SDL being initialised (the resampler path needs
// SDL_CreateAudioStream).

#include "test_harness.h"
#include "broaudio/engine.h"

#include <atomic>
#include <cmath>
#include <numbers>
#include <vector>

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;

static std::vector<float> ramp(int n) {
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) v[i] = static_cast<float>(i) * 0.001f;
    return v;
}

TEST(mic_tap_engine_rate_passthrough) {
    Engine e; e.initHeadless();

    std::vector<float> captured;
    int callCount = 0;
    auto id = e.addMicTap({}, [&](const float* s, int n) {
        captured.insert(captured.end(), s, s + n);
        callCount++;
    });
    ASSERT_TRUE(id != kInvalidMicTapId);

    auto in = ramp(96);
    e.injectMicSamples(in.data(), 96);
    ASSERT_EQ(callCount, 1);
    ASSERT_EQ(static_cast<int>(captured.size()), 96);
    for (int i = 0; i < 96; i++) ASSERT_NEAR(captured[i], in[i], 1e-6f);

    e.removeMicTap(id);
    PASS();
}

TEST(mic_tap_multiple_consumers_each_see_chunk) {
    Engine e; e.initHeadless();

    int aCalls = 0, bCalls = 0;
    int aSamples = 0, bSamples = 0;
    auto aId = e.addMicTap({}, [&](const float*, int n) { aCalls++; aSamples += n; });
    auto bId = e.addMicTap({}, [&](const float*, int n) { bCalls++; bSamples += n; });

    auto in = ramp(64);
    e.injectMicSamples(in.data(), 64);
    e.injectMicSamples(in.data(), 32);
    ASSERT_EQ(aCalls, 2);
    ASSERT_EQ(bCalls, 2);
    ASSERT_EQ(aSamples, 96);
    ASSERT_EQ(bSamples, 96);

    e.removeMicTap(aId);
    e.removeMicTap(bId);
    PASS();
}

TEST(mic_tap_remove_stops_delivery) {
    Engine e; e.initHeadless();

    int aCalls = 0, bCalls = 0;
    auto aId = e.addMicTap({}, [&](const float*, int) { aCalls++; });
    auto bId = e.addMicTap({}, [&](const float*, int) { bCalls++; });

    auto in = ramp(16);
    e.injectMicSamples(in.data(), 16);
    ASSERT_EQ(aCalls, 1);
    ASSERT_EQ(bCalls, 1);

    e.removeMicTap(aId);
    e.injectMicSamples(in.data(), 16);
    e.injectMicSamples(in.data(), 16);
    ASSERT_EQ(aCalls, 1);   // unchanged
    ASSERT_EQ(bCalls, 3);

    e.removeMicTap(bId);
    e.injectMicSamples(in.data(), 16);
    ASSERT_EQ(bCalls, 3);   // also unchanged after remove
    PASS();
}

TEST(mic_tap_agc_lifts_quiet_signal) {
    Engine e; e.initHeadless();

    MicTapConfig cfg;
    cfg.agc = true;
    cfg.agcCfg.targetPeak = 0.95f;
    cfg.agcCfg.halfLifeSec = 0.1f;     // fast convergence for the test
    cfg.agcCfg.noiseGate   = 0.001f;   // gate well below test peak
    cfg.agcCfg.maxGain     = 100.0f;

    float maxOut = 0.0f;
    auto id = e.addMicTap(cfg, [&](const float* s, int n) {
        for (int i = 0; i < n; i++) {
            float a = std::fabs(s[i]);
            if (a > maxOut) maxOut = a;
        }
    });

    // Feed several chunks of a quiet sine so the AGC converges.
    const int rate = e.sampleRate();
    const int chunk = 4096;
    std::vector<float> in(chunk);
    for (int c = 0; c < 8; c++) {
        for (int i = 0; i < chunk; i++) {
            in[i] = 0.05f * std::sin(2.0f * PI * 440.0f * (c * chunk + i) / rate);
        }
        e.injectMicSamples(in.data(), chunk);
    }
    // After convergence the output peak should be near target (0.95).
    ASSERT_TRUE(maxOut > 0.5f);

    e.removeMicTap(id);
    PASS();
}

TEST(mic_tap_chunk_frames_slices_stream) {
    Engine e; e.initHeadless();

    MicTapConfig cfg;
    cfg.chunkFrames = 160;

    std::vector<int> sizes;
    auto id = e.addMicTap(cfg, [&](const float*, int n) { sizes.push_back(n); });

    auto in = ramp(400);
    e.injectMicSamples(in.data(), 400);     // -> 2 chunks of 160, 80 buffered
    ASSERT_EQ(static_cast<int>(sizes.size()), 2);
    ASSERT_EQ(sizes[0], 160);
    ASSERT_EQ(sizes[1], 160);

    e.injectMicSamples(in.data(), 80);      // -> 1 more chunk, 0 buffered
    ASSERT_EQ(static_cast<int>(sizes.size()), 3);
    ASSERT_EQ(sizes[2], 160);

    e.removeMicTap(id);
    PASS();
}

TEST(mic_tap_stats_track_delivery) {
    Engine e; e.initHeadless();

    auto id = e.addMicTap({}, [](const float*, int) {});
    auto in = ramp(64);
    e.injectMicSamples(in.data(), 64);
    e.injectMicSamples(in.data(), 32);

    auto s = e.getMicTapStats(id);
    ASSERT_EQ(static_cast<int>(s.framesDelivered), 2);
    ASSERT_EQ(static_cast<int>(s.samplesDelivered), 96);
    ASSERT_TRUE(s.rollingPeak > 0.0f);

    e.removeMicTap(id);
    PASS();
}

TEST(mic_tap_invalid_inputs_return_invalid_id) {
    Engine e; e.initHeadless();
    ASSERT_EQ(e.addMicTap({}, nullptr), kInvalidMicTapId);
    MicTapConfig bad; bad.chunkFrames = -1;
    ASSERT_EQ(e.addMicTap(bad, [](const float*, int){}), kInvalidMicTapId);
    PASS();
}

int main() { return runAllTests(); }
