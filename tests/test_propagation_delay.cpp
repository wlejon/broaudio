// Propagation delay: one-shot onset = distance / c (world scale and speed of
// sound respected), the max-delay cap, the delayed tail keeps a finished
// one-shot alive, a moving looped source stays click-free, and Doppler comes
// from the line alone when it is on.

#include "test_harness.h"
#include "distance_test_util.h"

#include <cstdio>

using namespace broaudio;

namespace {

struct Rig {
    Engine e;
    int sr = 0;
    int clip = -1;
    Rig() {
        e.initHeadless();
        e.setMasterGain(0.5f);
        e.setLimiterEnabled(false);   // its lookahead would shift every onset
        e.setHeadModelEnabled(false);
        sr = e.sampleRate();
    }
    int burst() {
        std::vector<float> b(64, 0.0f);
        for (int i = 0; i < 32; i++) b[i] = 0.3f;
        clip = e.createClip(b.data(), static_cast<int>(b.size()), 1);
        return clip;
    }
    int spatial(int clipId, float z, bool loop = false) {
        int pb = e.playClip(clipId, 1.0f, loop);
        e.setPlaybackSpatialEnabled(pb, true);
        e.setPlaybackSpatialRolloff(pb, 0.0f);
        e.setPlaybackSpatialMaxDistance(pb, 100000.f);
        e.setPlaybackSpatialPosition(pb, 0.f, 0.f, z);
        e.setPlaybackSpatialPropagationDelay(pb, true);
        return pb;
    }
};

int onsetFrames(float metres, float mpu = 1.f, float c = 343.f, float maxDelay = -1.f) {
    Rig r;
    r.e.setSpatialMetresPerUnit(mpu);
    r.e.setSpatialSpeedOfSound(c);
    if (maxDelay >= 0.f) r.e.setSpatialMaxPropagationDelay(maxDelay);
    r.spatial(r.burst(), -metres / mpu);
    auto rec = dtest::left(dtest::record(r.e, r.sr * 2));
    return dtest::firstAbove(rec, 1e-3f);
}

} // namespace

TEST(onset_equals_distance_over_speed_of_sound) {
    const float sr = 44100.f;
    const float cases[] = {10.f, 50.f, 100.f, 150.f};
    for (float d : cases) {
        const int got = onsetFrames(d);
        const float want = d / 343.f * sr;
        std::printf("  %5.0f m: onset %d, expected %.1f frames\n", d, got, want);
        ASSERT_NEAR(static_cast<float>(got), want, 2.0f);
    }
    // World scale and speed of sound: 50 units x 2 m = 100 m; c = 686 halves it.
    ASSERT_NEAR(static_cast<float>(onsetFrames(100.f, 2.f)), 100.f / 343.f * sr, 2.0f);
    ASSERT_NEAR(static_cast<float>(onsetFrames(100.f, 1.f, 686.f)), 100.f / 686.f * sr, 2.0f);
    PASS();
}

TEST(max_delay_caps_the_onset) {
    const float sr = 44100.f;
    // 300 m is 0.875 s; the default cap is 0.5 s.
    ASSERT_NEAR(static_cast<float>(onsetFrames(300.f)), 0.5f * sr, 2.0f);
    ASSERT_NEAR(static_cast<float>(onsetFrames(300.f, 1.f, 343.f, 0.25f)), 0.25f * sr, 2.0f);
    // Raising the cap past the distance restores the true delay.
    ASSERT_NEAR(static_cast<float>(onsetFrames(300.f, 1.f, 343.f, 1.0f)), 300.f / 343.f * sr, 2.0f);
    PASS();
}

TEST(finished_one_shot_plays_out_its_delayed_tail) {
    Rig r;
    int pb = r.spatial(r.burst(), -100.f);
    const int delay = static_cast<int>(100.f / 343.f * r.sr);
    // Source content ends after 64 frames; the playback must stay alive until
    // the delayed copy has been heard, then finish.
    r.e.renderBlock(1024);
    ASSERT_TRUE(r.e.getPlaybackState(pb) == Engine::PlaybackState::Playing);
    r.e.renderBlock(delay);
    ASSERT_TRUE(r.e.getPlaybackState(pb) == Engine::PlaybackState::Playing);
    r.e.renderBlock(4096);
    ASSERT_TRUE(r.e.getPlaybackState(pb) != Engine::PlaybackState::Playing);
    PASS();
}

// A looped 440 Hz tone receding from 10 m at 100 m/s, position updated at
// 60 Hz. The delay line must move continuously: no sample-to-sample jump
// beyond what a sine of that amplitude can do, and the pitch ratio settles
// at 1 - v/c.
TEST(moving_looped_source_has_no_discontinuities) {
    Rig r;
    auto tone = dtest::sine(r.sr, 441.f, 0.25f, r.sr);   // 441 Hz: whole cycles per second, seamless loop
    int clip = r.e.createClip(tone.data(), static_cast<int>(tone.size()), 1);
    const float v = 100.f;
    r.e.setSpatialMaxPropagationDelay(1.0f);   // 210 m stays under the cap
    int pb = r.spatial(clip, -10.f, true);
    const int chunk = r.sr / 60;
    std::vector<float> ratios;
    auto rec = dtest::left(dtest::record(r.e, r.sr * 2, chunk, [&](int frame) {
        const float t = static_cast<float>(frame) / r.sr;
        r.e.setPlaybackSpatialPosition(pb, 0.f, 0.f, -(10.f + v * t));
        if (frame > r.sr) ratios.push_back(r.e.getPlaybackDopplerRatio(pb));
    }));
    // Amplitude reaching the output: measure it, then bound the slope.
    float peak = 0.f;
    for (int i = r.sr / 2; i < static_cast<int>(rec.size()); i++) peak = std::max(peak, std::fabs(rec[i]));
    float maxStep = 0.f, maxCurv = 0.f;
    for (int i = 2000; i < static_cast<int>(rec.size()); i++) {
        maxStep = std::max(maxStep, std::fabs(rec[i] - rec[i - 1]));
        maxCurv = std::max(maxCurv, std::fabs(rec[i] - 2.f * rec[i - 1] + rec[i - 2]));
    }
    const float w = 2.f * dtest::kPi * 441.f / r.sr;   // pitch falls, so this bounds the slope
    std::printf("  peak %.4f  max step %.5f (sine bound %.5f)  max 2nd diff %.6f (bound %.6f)\n",
                peak, maxStep, peak * w, maxCurv, peak * w * w);
    ASSERT_GT(peak, 0.01f);
    ASSERT_LT(maxStep, peak * w * 1.05f);
    ASSERT_LT(maxCurv, peak * w * w * 1.25f);
    float avg = 0.f;
    for (float x : ratios) avg += x;
    avg /= static_cast<float>(ratios.size());
    std::printf("  delay-line Doppler ratio %.4f (1 - v/c = %.4f)\n", avg, 1.f - v / 343.f);
    ASSERT_NEAR(avg, 1.f - v / 343.f, 0.01f);
    PASS();
}

TEST(doppler_is_not_counted_twice) {
    // A stationary source with a (stale) velocity: rate Doppler would shift
    // it, the delay line (distance constant) must not.
    Rig r;
    auto tone = dtest::sine(r.sr, 441.f, 0.25f, r.sr);
    int clip = r.e.createClip(tone.data(), static_cast<int>(tone.size()), 1);
    int pb = r.spatial(clip, -50.f, true);
    r.e.setPlaybackSpatialVelocity(pb, 0.f, 0.f, -60.f);
    r.e.renderBlock(r.sr / 2);
    ASSERT_NEAR(r.e.getPlaybackDopplerRatio(pb), 1.0f, 1e-3f);
    // Without the delay the velocity model applies as before.
    r.e.setPlaybackSpatialPropagationDelay(pb, false);
    r.e.renderBlock(1024);
    ASSERT_LT(r.e.getPlaybackDopplerRatio(pb), 0.9f);
    PASS();
}

TEST(setters_clamp_and_ignore_garbage) {
    Rig r;
    r.e.setSpatialSpeedOfSound(0.f);
    ASSERT_NEAR(r.e.spatialSpeedOfSound(), 1.f, 1e-6f);
    r.e.setSpatialSpeedOfSound(std::nanf(""));
    ASSERT_NEAR(r.e.spatialSpeedOfSound(), 1.f, 1e-6f);
    r.e.setSpatialMaxPropagationDelay(99.f);
    ASSERT_NEAR(r.e.spatialMaxPropagationDelay(), 10.f, 1e-6f);
    r.e.setSpatialMaxPropagationDelay(-1.f);
    ASSERT_NEAR(r.e.spatialMaxPropagationDelay(), 0.f, 1e-6f);
    // Unknown playback ids are no-ops.
    r.e.setPlaybackSpatialPropagationDelay(12345, true);
    r.e.setPlaybackSpatialAirAbsorption(12345, true);
    PASS();
}

int main() { return runAllTests(); }
