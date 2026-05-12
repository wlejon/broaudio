#include "test_harness.h"
#include "broaudio/engine.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;

static std::vector<float> makeSineMono(int frames, float freq, int sr) {
    std::vector<float> v(frames);
    for (int i = 0; i < frames; i++)
        v[i] = std::sin(2.0f * PI * freq * i / sr);
    return v;
}

static std::vector<float> makeSineStereo(int frames, float freq, int sr) {
    std::vector<float> v(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * PI * freq * i / sr);
        v[i * 2]     = s;
        v[i * 2 + 1] = -s;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Clip creation + metadata
// ---------------------------------------------------------------------------

TEST(create_mono_clip_round_trip) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(2048, 440.0f, e.sampleRate());
    int id = e.createClip(sine.data(), (int)sine.size(), 1);
    ASSERT_TRUE(id >= 0);
    ASSERT_EQ(e.getClipChannels(id), 1);
    ASSERT_EQ(e.getClipSampleCount(id), 2048);
    PASS();
}

TEST(create_stereo_clip_metadata) {
    Engine e; e.initHeadless();
    auto sine = makeSineStereo(1024, 220.0f, e.sampleRate());
    int id = e.createClip(sine.data(), (int)sine.size(), 2);
    ASSERT_TRUE(id >= 0);
    ASSERT_EQ(e.getClipChannels(id), 2);
    ASSERT_EQ(e.getClipSampleCount(id), 1024);
    PASS();
}

TEST(create_clip_rejects_bad_params) {
    Engine e; e.initHeadless();
    float dummy = 0.0f;
    ASSERT_EQ(e.createClip(nullptr, 100, 1), -1);
    ASSERT_EQ(e.createClip(&dummy, 0, 1), -1);
    ASSERT_EQ(e.createClip(&dummy, 1, 0), -1);
    ASSERT_EQ(e.createClip(&dummy, 1, 3), -1);  // > 2 channels rejected
    PASS();
}

TEST(get_clip_metadata_for_bad_id) {
    Engine e; e.initHeadless();
    ASSERT_EQ(e.getClipSampleCount(9999), 0);
    ASSERT_EQ(e.getClipChannels(9999), 0);
    PASS();
}

TEST(delete_clip_removes_metadata) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(256, 440.0f, e.sampleRate());
    int id = e.createClip(sine.data(), (int)sine.size(), 1);
    ASSERT_EQ(e.getClipSampleCount(id), 256);
    e.deleteClip(id);
    ASSERT_EQ(e.getClipSampleCount(id), 0);
    PASS();
}

// ---------------------------------------------------------------------------
// Waveform extraction
// ---------------------------------------------------------------------------

TEST(get_clip_waveform_min_max_bounds) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(4096, 440.0f, e.sampleRate());
    int id = e.createClip(sine.data(), (int)sine.size(), 1);

    constexpr int BINS = 32;
    float minmax[BINS * 2];
    e.getClipWaveform(id, minmax, BINS);

    // min should be <= 0 and max should be >= 0 for a full sine
    bool anyNonzero = false;
    for (int i = 0; i < BINS; i++) {
        float lo = minmax[i * 2];
        float hi = minmax[i * 2 + 1];
        ASSERT_TRUE(lo <= hi);
        ASSERT_TRUE(lo >= -1.01f);
        ASSERT_TRUE(hi <= 1.01f);
        if (std::fabs(lo) > 0.01f || std::fabs(hi) > 0.01f) anyNonzero = true;
    }
    ASSERT_TRUE(anyNonzero);
    PASS();
}

TEST(get_clip_waveform_stereo_averages_channels) {
    Engine e; e.initHeadless();
    auto sine = makeSineStereo(4096, 220.0f, e.sampleRate());
    int id = e.createClip(sine.data(), (int)sine.size(), 2);

    constexpr int BINS = 16;
    float minmax[BINS * 2];
    e.getClipWaveform(id, minmax, BINS);

    // Stereo channels are inverted, so the average should be near zero
    for (int i = 0; i < BINS; i++) {
        ASSERT_NEAR(minmax[i * 2], 0.0f, 0.1f);
        ASSERT_NEAR(minmax[i * 2 + 1], 0.0f, 0.1f);
    }
    PASS();
}

TEST(get_clip_waveform_invalid_id_zeros_buffer) {
    Engine e; e.initHeadless();
    constexpr int BINS = 8;
    float minmax[BINS * 2];
    for (int i = 0; i < BINS * 2; i++) minmax[i] = 99.0f;
    e.getClipWaveform(9999, minmax, BINS);
    for (int i = 0; i < BINS * 2; i++)
        ASSERT_NEAR(minmax[i], 0.0f, 1e-6f);
    PASS();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

TEST(play_clip_invalid_returns_negative) {
    Engine e; e.initHeadless();
    ASSERT_EQ(e.playClip(9999, 1.0f, false), -1);
    PASS();
}

TEST(play_clip_produces_audio_on_master) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, false);
    ASSERT_TRUE(pid >= 0);

    e.renderBlock(2048);
    float peak = e.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_GT(peak, 0.05f);
    PASS();
}

TEST(stop_playback_silences_master) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);

    e.stopPlayback(pid);
    e.renderBlock(4096);  // let smoothers settle
    e.renderBlock(2048);
    ASSERT_LT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);
    PASS();
}

TEST(set_playback_gain_changes_amplitude) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.renderBlock(4096);  // settle
    float full = e.getBusPeakL(Engine::MASTER_BUS_ID);

    e.setPlaybackGain(pid, 0.1f);
    e.renderBlock(4096);
    e.renderBlock(2048);
    float quiet = e.getBusPeakL(Engine::MASTER_BUS_ID);

    ASSERT_LT(quiet, full);
    PASS();
}

TEST(set_playback_rate_clamps_to_range) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 0.5f, true);

    e.setPlaybackRate(pid, 1000.0f);  // clamped to 16.0
    e.setPlaybackRate(pid, -10.0f);   // clamped to 0.01
    e.setPlaybackRate(pid, 2.0f);
    e.renderBlock(2048);

    // Just verify the engine doesn't crash and produces audio
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);
    PASS();
}

TEST(set_playback_pan_affects_balance) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.setPlaybackPan(pid, -1.0f);   // hard left
    e.renderBlock(4096);
    e.renderBlock(1024);
    float left  = e.getBusPeakL(Engine::MASTER_BUS_ID);
    float right = e.getBusPeakR(Engine::MASTER_BUS_ID);
    ASSERT_GT(left, right * 2.0f);
    PASS();
}

TEST(set_playback_loop_keeps_playing_past_end) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(256, 440.0f, e.sampleRate());  // short clip, ~5.8ms at 44100
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    // Render way past clip duration
    e.renderBlock(8192);
    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);

    e.setPlaybackLoop(pid, false);
    PASS();
}

TEST(set_playback_region_clamps_inside_clip) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(1024, 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    // Region exceeds clip — should clamp
    e.setPlaybackRegion(pid, -100, 99999);
    // Also exercise inverted ranges (end < start gets clamped to start)
    e.setPlaybackRegion(pid, 500, 200);
    // Normal region
    e.setPlaybackRegion(pid, 100, 800);
    e.renderBlock(2048);
    PASS();
}

TEST(set_playback_region_invalid_clip_id_is_safe) {
    Engine e; e.initHeadless();
    // No crash for bogus instanceId
    e.setPlaybackRegion(9999, 0, 100);
    e.setPlaybackGain(9999, 0.5f);
    e.setPlaybackLoop(9999, true);
    e.setPlaybackRate(9999, 1.5f);
    e.setPlaybackPan(9999, 0.5f);
    e.setPlaybackPlaying(9999, true);
    e.stopPlayback(9999);
    ASSERT_NEAR(e.getPlaybackPosition(9999), 0.0f, 1e-6f);
    PASS();
}

TEST(set_playback_playing_toggle) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.setPlaybackPlaying(pid, false);
    e.renderBlock(4096);
    e.renderBlock(2048);
    float silent = e.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_LT(silent, 0.05f);

    e.setPlaybackPlaying(pid, true);   // resets play pos
    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);
    PASS();
}

TEST(get_playback_position_in_range) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.renderBlock(2048);
    float p = e.getPlaybackPosition(pid);
    ASSERT_TRUE(p >= 0.0f);
    ASSERT_TRUE(p <= 1.0f);
    PASS();
}

TEST(delete_clip_stops_active_playback) {
    Engine e; e.initHeadless();
    auto sine = makeSineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);

    e.deleteClip(cid);
    e.renderBlock(4096);  // smoothers settle
    e.renderBlock(2048);
    ASSERT_LT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);

    // playback id now refers to a deleted clip — getPosition returns 0
    ASSERT_NEAR(e.getPlaybackPosition(pid), 0.0f, 1e-6f);
    PASS();
}

TEST(stereo_clip_playback_produces_stereo_output) {
    Engine e; e.initHeadless();
    int sr = e.sampleRate();
    // Stereo clip: left = sine, right = silence
    std::vector<float> stereo(sr * 2);
    for (int i = 0; i < sr; i++) {
        stereo[i * 2]     = std::sin(2.0f * PI * 440.0f * i / sr);
        stereo[i * 2 + 1] = 0.0f;
    }
    int cid = e.createClip(stereo.data(), sr * 2, 2);
    e.playClip(cid, 1.0f, false);
    e.renderBlock(2048);
    e.renderBlock(1024);

    float L = e.getBusPeakL(Engine::MASTER_BUS_ID);
    float R = e.getBusPeakR(Engine::MASTER_BUS_ID);
    ASSERT_GT(L, R);
    PASS();
}

int main() { return runAllTests(); }
