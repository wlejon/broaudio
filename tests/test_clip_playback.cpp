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
    auto wf = e.getClipWaveform(id, BINS);
    ASSERT_EQ(wf.size(), static_cast<size_t>(BINS * 2));

    // min should be <= 0 and max should be >= 0 for a full sine
    bool anyNonzero = false;
    for (int i = 0; i < BINS; i++) {
        float lo = wf[i * 2];
        float hi = wf[i * 2 + 1];
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
    auto wf = e.getClipWaveform(id, BINS);
    ASSERT_EQ(wf.size(), static_cast<size_t>(BINS * 2));

    // Stereo channels are inverted, so the average should be near zero
    for (int i = 0; i < BINS; i++) {
        ASSERT_NEAR(wf[i * 2], 0.0f, 0.1f);
        ASSERT_NEAR(wf[i * 2 + 1], 0.0f, 0.1f);
    }
    PASS();
}

TEST(get_clip_waveform_invalid_id_zeros_buffer) {
    Engine e; e.initHeadless();
    constexpr int BINS = 8;
    auto wf = e.getClipWaveform(9999, BINS);
    ASSERT_EQ(wf.size(), static_cast<size_t>(BINS * 2));
    for (int i = 0; i < BINS * 2; i++)
        ASSERT_NEAR(wf[i], 0.0f, 1e-6f);
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

TEST(get_playback_position_seconds_tracks_render) {
    Engine e; e.initHeadless();
    const int sr = e.sampleRate();
    auto sine = makeSineMono(sr * 2, 440.0f, sr);
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, false);

    e.renderBlock(sr / 10);  // 100 ms
    double p = e.getPlaybackPositionSeconds(pid);
    ASSERT_NEAR(p, 0.1, 0.005);
    PASS();
}

TEST(seek_playback_jumps_clip_cursor) {
    Engine e; e.initHeadless();
    const int sr = e.sampleRate();
    auto sine = makeSineMono(sr * 2, 440.0f, sr);
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, false);

    e.renderBlock(1024);
    e.seekPlayback(pid, 1.0);
    ASSERT_NEAR(e.getPlaybackPositionSeconds(pid), 1.0, 0.001);

    // Rendering continues from the new cursor.
    e.renderBlock(sr / 10);
    ASSERT_NEAR(e.getPlaybackPositionSeconds(pid), 1.1, 0.005);
    PASS();
}

TEST(seek_playback_clamps_to_region) {
    Engine e; e.initHeadless();
    const int sr = e.sampleRate();
    auto sine = makeSineMono(sr, 440.0f, sr);
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.seekPlayback(pid, 100.0);   // way past the end — clamps inside
    double p = e.getPlaybackPositionSeconds(pid);
    ASSERT_TRUE(p <= 1.0);
    ASSERT_GT(p, 0.99);

    e.seekPlayback(pid, -5.0);    // negative — clamps to 0
    ASSERT_NEAR(e.getPlaybackPositionSeconds(pid), 0.0, 0.001);
    PASS();
}

TEST(seek_playback_invalid_id_is_safe) {
    Engine e; e.initHeadless();
    e.seekPlayback(9999, 1.0);
    ASSERT_NEAR(e.getPlaybackPositionSeconds(9999), 0.0, 1e-9);
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

// ---------------------------------------------------------------------------
// Sample-accurate scheduled playback (playClipAt) — the streaming join
// ---------------------------------------------------------------------------

TEST(play_clip_at_invalid_returns_negative) {
    Engine e; e.initHeadless();
    ASSERT_EQ(e.playClipAt(9999, 0.5, 1.0f, false), -1);
    PASS();
}

TEST(play_clip_at_zero_plays_immediately) {
    Engine e; e.initHeadless();
    int sr = e.sampleRate();
    auto sine = makeSineMono(sr, 440.0f, sr);
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    e.playClipAt(cid, 0.0, 1.0f, false);              // when <= now → immediate
    e.renderBlock(512);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.2f);
    PASS();
}

TEST(play_clip_at_is_silent_before_scheduled_sample) {
    Engine e; e.initHeadless();
    int sr = e.sampleRate();
    auto sine = makeSineMono(sr, 440.0f, sr);
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);

    const int startSample = 1000;
    e.startRecording();
    int pid = e.playClipAt(cid, (double)startSample / sr, 0.5f, false);
    ASSERT_TRUE(pid >= 0);
    e.renderBlock(4096);
    e.stopRecording();
    auto out = e.getRecordBuffer();                   // mono, one float per frame
    ASSERT_TRUE((int)out.size() >= 4096);

    // Exactly silent before the scheduled sample. (The master limiter's lookahead
    // only ever DELAYS audio further, never advances it, so a clean zero here is
    // the real guarantee that a scheduled clip can't leak in early.)
    float before = 0.0f;
    for (int i = 0; i < startSample; i++) before = std::max(before, std::fabs(out[i]));
    ASSERT_NEAR(before, 0.0f, 1e-6f);
    // ...and clearly audible somewhere after it.
    float after = 0.0f;
    for (int i = startSample; i < 4096; i++) after = std::max(after, std::fabs(out[i]));
    ASSERT_GT(after, 0.05f);
    PASS();
}

TEST(scheduled_clips_join_gaplessly_at_the_seam) {
    Engine e; e.initHeadless();
    int sr = e.sampleRate();
    const int N = 1500;
    auto sine = makeSineMono(N, 440.0f, sr);
    int a = e.createClip(sine.data(), N, 1);
    int b = e.createClip(sine.data(), N, 1);

    // Two identical clips, B starting exactly where A ends. If the schedule were
    // off by even a block, the seam would show a silent gap — this is the
    // streaming chunk-join the lab relies on.
    e.startRecording();
    e.playClipAt(a, 0.0, 0.5f, false);                // covers [0, N)
    e.playClipAt(b, (double)N / sr, 0.5f, false);     // covers [N, 2N)
    e.renderBlock(2 * N + 512);
    e.stopRecording();
    auto out = e.getRecordBuffer();
    ASSERT_TRUE((int)out.size() >= 2 * N);

    // Interior region, past the limiter's lookahead onset. Self-calibrate the
    // silence floor to the actual level, then assert there is no long silent run:
    // a continuous signal only dips near its zero crossings (a handful of samples
    // every half-period); a real scheduling gap would be hundreds of samples.
    const int lo = 400, hi = 2 * N;
    float peak = 0.0f;
    for (int i = lo; i < hi; i++) peak = std::max(peak, std::fabs(out[i]));
    ASSERT_GT(peak, 0.02f);
    const float floorLvl = 0.15f * peak;
    int run = 0, maxRun = 0;
    for (int i = lo; i < hi; i++) {
        if (std::fabs(out[i]) < floorLvl) { run++; maxRun = std::max(maxRun, run); }
        else run = 0;
    }
    ASSERT_LT(maxRun, 80);   // no gap at the A→B seam
    PASS();
}

int main() { return runAllTests(); }
