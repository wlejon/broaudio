#include "test_harness.h"
#include "broaudio/engine.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;

static std::vector<float> sineMono(int frames, float freq, int sr) {
    std::vector<float> v(frames);
    for (int i = 0; i < frames; i++)
        v[i] = std::sin(2.0f * PI * freq * i / sr);
    return v;
}

// ---------------------------------------------------------------------------
// Listener position/orientation
// ---------------------------------------------------------------------------

TEST(set_listener_position_stores) {
    Engine e; e.initHeadless();
    e.setListenerPosition(1.5f, -2.0f, 3.0f);
    auto p = e.listener().position();
    ASSERT_NEAR(p.x, 1.5f, 1e-6f);
    ASSERT_NEAR(p.y, -2.0f, 1e-6f);
    ASSERT_NEAR(p.z, 3.0f, 1e-6f);
    PASS();
}

TEST(set_listener_orientation_stores) {
    Engine e; e.initHeadless();
    e.setListenerOrientation(1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f);
    auto f = e.listener().forward();
    auto u = e.listener().up();
    ASSERT_NEAR(f.x, 1.0f, 1e-6f);
    ASSERT_NEAR(f.z, 0.0f, 1e-6f);
    ASSERT_NEAR(u.y, 1.0f, 1e-6f);
    PASS();
}

// ---------------------------------------------------------------------------
// HeadModel API
// ---------------------------------------------------------------------------

TEST(head_model_setters_propagate) {
    Engine e; e.initHeadless();
    e.setHeadModelEnabled(false);
    ASSERT_FALSE(e.headModel().enabled.load());

    e.setHeadModelEnabled(true);
    ASSERT_TRUE(e.headModel().enabled.load());

    e.setHeadModelIldStrength(0.5f);
    ASSERT_NEAR(e.headModel().ildStrength.load(), 0.5f, 1e-6f);

    e.setHeadModelBehindAttenuation(0.3f);
    ASSERT_NEAR(e.headModel().behindAttenuation.load(), 0.3f, 1e-6f);

    e.setHeadModelNearCutoff(15000.0f, 1500.0f);
    ASSERT_NEAR(e.headModel().nearCutoffFront.load(), 15000.0f, 0.1f);
    ASSERT_NEAR(e.headModel().nearCutoffBehind.load(), 1500.0f, 0.1f);

    e.setHeadModelFarCutoffRatio(0.7f);
    ASSERT_NEAR(e.headModel().farCutoffRatio.load(), 0.7f, 1e-6f);

    e.setHeadModelElevation(3000.0f, 1000.0f);
    ASSERT_NEAR(e.headModel().elevationNear.load(), 3000.0f, 0.1f);
    ASSERT_NEAR(e.headModel().elevationFar.load(), 1000.0f, 0.1f);

    e.setHeadModelCutoffRange(100.0f, 18000.0f);
    ASSERT_NEAR(e.headModel().minCutoff.load(), 100.0f, 0.1f);
    ASSERT_NEAR(e.headModel().maxCutoff.load(), 18000.0f, 0.1f);
    PASS();
}

// ---------------------------------------------------------------------------
// Voice spatial
// ---------------------------------------------------------------------------

TEST(voice_spatial_setters_dont_crash_on_bad_id) {
    Engine e; e.initHeadless();
    e.setVoiceSpatialEnabled(9999, true);
    e.setVoiceSpatialPosition(9999, 1.0f, 2.0f, 3.0f);
    e.setVoiceSpatialRefDistance(9999, 1.0f);
    e.setVoiceSpatialMaxDistance(9999, 50.0f);
    e.setVoiceSpatialRolloff(9999, 1.5f);
    e.setVoiceSpatialDistanceModel(9999, DistanceModel::Linear);
    PASS();
}

TEST(voice_spatial_left_position_pans_left) {
    Engine e; e.initHeadless();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceSpatialEnabled(v, true);
    e.setVoiceSpatialPosition(v, -5.0f, 0.0f, 0.0f);  // listener faces -Z, so -X is left
    e.setVoiceSpatialRefDistance(v, 1.0f);
    e.setVoiceSpatialMaxDistance(v, 100.0f);
    e.setVoiceSpatialRolloff(v, 1.0f);
    e.setVoiceSpatialDistanceModel(v, DistanceModel::Inverse);
    e.startVoice(v, 0.0);
    // Disable head model to keep test isolated to the pan/gain path
    e.setHeadModelEnabled(false);

    e.renderBlock(4096);
    e.renderBlock(2048);

    float L = e.getBusPeakL(Engine::MASTER_BUS_ID);
    float R = e.getBusPeakR(Engine::MASTER_BUS_ID);
    ASSERT_GT(L, R);
    PASS();
}

TEST(voice_spatial_distance_models_attenuate) {
    // Verify that increasing distance reduces output amplitude
    Engine e; e.initHeadless();
    e.setHeadModelEnabled(false);

    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceSpatialEnabled(v, true);
    e.setVoiceSpatialRefDistance(v, 1.0f);
    e.setVoiceSpatialMaxDistance(v, 100.0f);
    e.setVoiceSpatialRolloff(v, 1.0f);
    e.setVoiceSpatialDistanceModel(v, DistanceModel::Inverse);
    e.setVoiceSpatialPosition(v, 0.0f, 0.0f, -1.0f);  // very close (1 unit ahead)
    e.startVoice(v, 0.0);

    e.renderBlock(4096);
    e.renderBlock(1024);
    float near = std::max(e.getBusPeakL(Engine::MASTER_BUS_ID),
                          e.getBusPeakR(Engine::MASTER_BUS_ID));

    e.setVoiceSpatialPosition(v, 0.0f, 0.0f, -50.0f);
    e.renderBlock(4096);
    e.renderBlock(1024);
    float far = std::max(e.getBusPeakL(Engine::MASTER_BUS_ID),
                         e.getBusPeakR(Engine::MASTER_BUS_ID));

    ASSERT_GT(near, far * 2.0f);
    PASS();
}

TEST(voice_spatial_linear_model) {
    Engine e; e.initHeadless();
    e.setHeadModelEnabled(false);
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceSpatialEnabled(v, true);
    e.setVoiceSpatialDistanceModel(v, DistanceModel::Linear);
    e.setVoiceSpatialRefDistance(v, 1.0f);
    e.setVoiceSpatialMaxDistance(v, 10.0f);
    e.setVoiceSpatialRolloff(v, 1.0f);
    e.setVoiceSpatialPosition(v, 0.0f, 0.0f, -5.0f);
    e.startVoice(v, 0.0);

    e.renderBlock(4096);
    // Should produce some audio
    ASSERT_GT(std::max(e.getBusPeakL(Engine::MASTER_BUS_ID),
                       e.getBusPeakR(Engine::MASTER_BUS_ID)), 0.01f);
    PASS();
}

TEST(voice_spatial_exponential_model) {
    Engine e; e.initHeadless();
    e.setHeadModelEnabled(false);
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceSpatialEnabled(v, true);
    e.setVoiceSpatialDistanceModel(v, DistanceModel::Exponential);
    e.setVoiceSpatialRefDistance(v, 1.0f);
    e.setVoiceSpatialMaxDistance(v, 100.0f);
    e.setVoiceSpatialRolloff(v, 2.0f);
    e.setVoiceSpatialPosition(v, 0.0f, 0.0f, -1.0f);
    e.startVoice(v, 0.0);
    e.renderBlock(4096);
    ASSERT_GT(std::max(e.getBusPeakL(Engine::MASTER_BUS_ID),
                       e.getBusPeakR(Engine::MASTER_BUS_ID)), 0.01f);
    PASS();
}

// ---------------------------------------------------------------------------
// Playback spatial
// ---------------------------------------------------------------------------

TEST(playback_spatial_setters_dont_crash_on_bad_id) {
    Engine e; e.initHeadless();
    e.setPlaybackSpatialEnabled(9999, true);
    e.setPlaybackSpatialPosition(9999, 1.0f, 2.0f, 3.0f);
    e.setPlaybackSpatialRefDistance(9999, 1.0f);
    e.setPlaybackSpatialMaxDistance(9999, 50.0f);
    e.setPlaybackSpatialRolloff(9999, 1.0f);
    e.setPlaybackSpatialDistanceModel(9999, DistanceModel::Linear);
    PASS();
}

TEST(playback_spatial_right_position_pans_right) {
    Engine e; e.initHeadless();
    // Spatial routing forces pan to center; L/R differences come from the head model.
    e.setHeadModelEnabled(true);
    e.setHeadModelIldStrength(0.9f);
    auto sine = sineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.setPlaybackSpatialEnabled(pid, true);
    e.setPlaybackSpatialDistanceModel(pid, DistanceModel::Inverse);
    e.setPlaybackSpatialRefDistance(pid, 1.0f);
    e.setPlaybackSpatialMaxDistance(pid, 100.0f);
    e.setPlaybackSpatialRolloff(pid, 1.0f);
    e.setPlaybackSpatialPosition(pid, 5.0f, 0.0f, 0.0f);  // right of listener

    e.renderBlock(4096);
    e.renderBlock(2048);

    float L = e.getBusPeakL(Engine::MASTER_BUS_ID);
    float R = e.getBusPeakR(Engine::MASTER_BUS_ID);
    ASSERT_GT(R, L);
    PASS();
}

TEST(voice_spatial_with_head_model_enabled_still_produces_audio) {
    Engine e; e.initHeadless();
    e.setHeadModelEnabled(true);
    e.setHeadModelIldStrength(0.8f);
    e.setHeadModelBehindAttenuation(0.4f);

    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceSpatialEnabled(v, true);
    e.setVoiceSpatialDistanceModel(v, DistanceModel::Inverse);
    e.setVoiceSpatialRefDistance(v, 1.0f);
    e.setVoiceSpatialMaxDistance(v, 100.0f);
    e.setVoiceSpatialRolloff(v, 1.0f);
    // Position to one side so ILD and pan are active
    e.setVoiceSpatialPosition(v, 2.0f, 0.0f, -1.0f);
    e.startVoice(v, 0.0);

    e.renderBlock(4096);
    e.renderBlock(2048);
    ASSERT_GT(std::max(e.getBusPeakL(Engine::MASTER_BUS_ID),
                       e.getBusPeakR(Engine::MASTER_BUS_ID)), 0.01f);
    PASS();
}

// ---------------------------------------------------------------------------
// Doppler
// ---------------------------------------------------------------------------

// Spatialized looping clip playback at `pos` with velocity `vel`.
static int makeDopplerPlayback(Engine& e, float px, float py, float pz,
                               float vx, float vy, float vz) {
    auto sine = sineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);
    e.setPlaybackSpatialEnabled(pid, true);
    e.setPlaybackSpatialPosition(pid, px, py, pz);
    e.setPlaybackSpatialVelocity(pid, vx, vy, vz);
    return pid;
}

TEST(doppler_approaching_source_raises_ratio) {
    Engine e; e.initHeadless();
    // Source 10 units in front, moving straight at the listener at 10 u/s:
    // ratio = 343 / (343 - 10).
    int pid = makeDopplerPlayback(e, 0, 0, -10, 0, 0, 10);
    e.renderBlock(512);
    ASSERT_NEAR(e.getPlaybackDopplerRatio(pid), 343.0f / 333.0f, 1e-3f);
    PASS();
}

TEST(doppler_receding_source_lowers_ratio) {
    Engine e; e.initHeadless();
    int pid = makeDopplerPlayback(e, 0, 0, -10, 0, 0, -10);
    e.renderBlock(512);
    ASSERT_NEAR(e.getPlaybackDopplerRatio(pid), 343.0f / 353.0f, 1e-3f);
    PASS();
}

TEST(doppler_listener_velocity_contributes) {
    Engine e; e.initHeadless();
    // Listener moving toward a static source: ratio = (343 + 5) / 343.
    int pid = makeDopplerPlayback(e, 0, 0, -10, 0, 0, 0);
    e.setListenerVelocity(0, 0, -5);
    e.renderBlock(512);
    ASSERT_NEAR(e.getPlaybackDopplerRatio(pid), 348.0f / 343.0f, 1e-3f);
    PASS();
}

TEST(doppler_factor_zero_disables) {
    Engine e; e.initHeadless();
    int pid = makeDopplerPlayback(e, 0, 0, -10, 0, 0, 10);
    e.setDopplerFactor(0.0f);
    e.renderBlock(512);
    ASSERT_NEAR(e.getPlaybackDopplerRatio(pid), 1.0f, 1e-6f);
    PASS();
}

TEST(doppler_ratio_clamps_to_octave) {
    Engine e; e.initHeadless();
    // Absurd speeds: projections clamp to ±0.9c first, then the ratio clamps
    // to [0.5, 2.0]. Approach saturates the upper clamp (343/34.3 = 10 → 2);
    // recede floors at 1/1.9 ≈ 0.5263 from the projection clamp alone.
    int up = makeDopplerPlayback(e, 0, 0, -10, 0, 0, 1000);
    int dn = makeDopplerPlayback(e, 0, 0, 10, 0, 0, 1000);
    e.renderBlock(512);
    ASSERT_NEAR(e.getPlaybackDopplerRatio(up), 2.0f, 1e-6f);
    ASSERT_NEAR(e.getPlaybackDopplerRatio(dn), 1.0f / 1.9f, 1e-4f);
    PASS();
}

TEST(doppler_shifts_playback_consumption_rate) {
    Engine e; e.initHeadless();
    const int sr = e.sampleRate();
    // Approaching source: cursor should advance ~ratio × realtime.
    int pid = makeDopplerPlayback(e, 0, 0, -10, 0, 0, 10);
    e.renderBlock(512);  // let the ratio latch before the measured window
    double before = e.getPlaybackPositionSeconds(pid);
    e.renderBlock(sr / 2);
    double advanced = e.getPlaybackPositionSeconds(pid) - before;
    ASSERT_NEAR(advanced, 0.5 * (343.0 / 333.0), 0.01);
    PASS();
}

TEST(doppler_voice_ratio_tracks_motion) {
    Engine e; e.initHeadless();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceSpatialEnabled(v, true);
    e.setVoiceSpatialPosition(v, 0, 0, -10);
    e.setVoiceSpatialVelocity(v, 0, 0, 10);
    e.startVoice(v, 0.0);
    e.renderBlock(512);
    ASSERT_NEAR(e.getVoiceDopplerRatio(v), 343.0f / 333.0f, 1e-3f);
    PASS();
}

int main() { return runAllTests(); }
