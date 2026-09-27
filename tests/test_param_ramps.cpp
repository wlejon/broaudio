// Ramps on setPlaybackGain / setPlaybackSend / setBusGain: a stated ramp is
// linear and lands on its target at the stated time; without one the change
// is de-zippered (no step larger than the one-pole allows).

#include "test_harness.h"
#include "distance_test_util.h"

#include <cstdio>

using namespace broaudio;

namespace {

// A constant (DC) looping clip makes the output a direct picture of the gain.
struct Rig {
    Engine e;
    int sr = 0;
    int dc = -1;
    Rig() {
        e.initHeadless();
        // Master gain stays at its snapped default (0.5): a DC of 0.5 reaches
        // the recording at 0.25 x playback gain x pan.
        e.setLimiterEnabled(false);
        sr = e.sampleRate();
        std::vector<float> v(4096, 0.5f);
        dc = e.createClip(v.data(), static_cast<int>(v.size()), 1);
    }
};

// Check rec[from .. from+len) is a straight line from a to b (inclusive of the
// end sample at from+len-1 reaching b), then flat at b for `hold` samples.
// Levels are checked to 1e-3 (the expected values use std::cos for the pan
// law) and the shape to 1e-4 against the measured plateau: a scale error is
// not a zipper, a late or early landing is.
bool isRamp(const std::vector<float>& x, int from, int len, float a, float b, int hold) {
    double plateau = 0.0;
    for (int i = 0; i < hold; i++) plateau += x[from + len + i];
    plateau /= hold;
    const float scale = b != 0.f ? static_cast<float>(plateau) / b : 1.f;
    float worst = 0.f;
    int at = 0;
    for (int i = 0; i < len + hold; i++) {
        const float want = scale * (i < len ? a + (b - a) * static_cast<float>(i + 1) / static_cast<float>(len) : b);
        const float dev = std::fabs(x[from + i] - want);
        if (dev > worst) { worst = dev; at = i; }
    }
    std::printf("  ramp %d samples %.4f -> %.4f (level scale %.5f): worst shape deviation %.2e at +%d\n",
                len, a, b, scale, worst, at);
    return std::fabs(scale - 1.f) < 1e-3f && worst < 1e-4f * std::max(std::fabs(a), std::fabs(b)) + 1e-6f;
}

float maxStep(const std::vector<float>& x, int from, int to) {
    float m = 0.f;
    int at = from;
    for (int i = from + 1; i < to; i++) {
        if (std::fabs(x[i] - x[i - 1]) > m) { m = std::fabs(x[i] - x[i - 1]); at = i; }
    }
    std::printf("  max step %.3g at +%d (%.6f -> %.6f)\n", m, at - from, x[at - 1], x[at]);
    return m;
}

} // namespace

TEST(playback_gain_ramp_is_linear_and_lands_on_time) {
    Rig r;
    int pb = r.e.playClip(r.dc, 0.2f, true);
    const float pan = std::cos(0.5f * 1.5707963f);   // centre, equal power
    const int rampFrames = r.sr / 2;                 // 0.5 s
    auto rec = dtest::left(dtest::record(r.e, r.sr * 2, 512, [&](int frame) {
        if (frame == 512 * 20) r.e.setPlaybackGain(pb, 1.0f, 0.5f);
    }));
    const int start = 512 * 20;
    ASSERT_TRUE(isRamp(rec, start, rampFrames, 0.25f * 0.2f * pan, 0.25f * 1.0f * pan, 4096));
    // Linear: the step is constant, no zipper.
    const float step = 0.25f * 0.8f * pan / rampFrames;
    ASSERT_NEAR(maxStep(rec, start, start + rampFrames), step, step * 0.02f);
    PASS();
}

TEST(ramp_set_before_first_mix_starts_from_previous_gain) {
    Rig r;
    int pb = r.e.playClip(r.dc, 0.0f, true);
    r.e.setPlaybackGain(pb, 1.0f, 0.1f);   // before any block: a fade-in
    auto rec = dtest::left(dtest::record(r.e, r.sr / 2));
    const float pan = std::cos(0.5f * 1.5707963f);
    ASSERT_TRUE(isRamp(rec, 0, r.sr / 10, 0.0f, 0.25f * pan, 1000));
    PASS();
}

TEST(gain_without_ramp_is_dezippered) {
    Rig r;
    int pb = r.e.playClip(r.dc, 0.0f, true);
    auto rec = dtest::left(dtest::record(r.e, r.sr / 2, 512, [&](int frame) {
        if (frame == 5120) r.e.setPlaybackGain(pb, 1.0f);
    }));
    const float pan = std::cos(0.5f * 1.5707963f);
    const float full = 0.25f * pan;
    // A one-pole approach: the largest step is the first, coeff x full, far
    // below a hard step; ~95 % after 5 ms.
    const float step = maxStep(rec, 5120 - 1, 5120 + 2000);
    std::printf("  de-zip largest step %.4f of %.4f\n", step, full);
    ASSERT_LT(step, 0.03f * full);
    ASSERT_NEAR(rec[5120 + static_cast<int>(0.005f * r.sr)], 0.95f * full, 0.02f * full);
    ASSERT_NEAR(rec[5120 + 4000], full, 1e-3f * full);
    PASS();
}

TEST(send_ramp_is_linear_and_lands_on_time) {
    Rig r;
    // The direct path goes to a muted bus; only the send reaches master.
    int dry = r.e.createBus();
    r.e.setBusMuted(dry, true);
    int ret = r.e.createBus();
    int pb = r.e.playClip(r.dc, 1.0f, true);
    r.e.setPlaybackBus(pb, dry);
    r.e.setPlaybackSend(pb, ret, 0.0f);
    const int rampFrames = static_cast<int>(0.3f * r.sr);
    auto rec = dtest::left(dtest::record(r.e, r.sr, 256, [&](int frame) {
        if (frame == 256 * 10) r.e.setPlaybackSend(pb, ret, 0.8f, 0.3f);
    }));
    // Send tap is post pan; the return bus mixes in at unity pan (balance at
    // centre passes L as L * cos(pi/4) + R * (1 - sin(pi/4)) = the sum of both).
    const float pan = std::cos(0.5f * 1.5707963f);
    const float busPan = pan + (1.0f - pan);
    const float full = 0.25f * pan * 0.8f * busPan;
    ASSERT_TRUE(isRamp(rec, 256 * 10, rampFrames, 0.0f, full, 2000));
    PASS();
}

TEST(bus_gain_ramp_is_linear_and_lands_on_time) {
    Rig r;
    int bus = r.e.createBus();
    r.e.setBusGain(bus, 0.5f);
    int pb = r.e.playClip(r.dc, 1.0f, true);
    r.e.setPlaybackBus(pb, bus);
    const int rampFrames = static_cast<int>(0.4f * r.sr);
    auto rec = dtest::left(dtest::record(r.e, r.sr, 441, [&](int frame) {
        if (frame == 441 * 10) r.e.setBusGain(bus, 1.5f, 0.4f);
    }));
    const float pan = std::cos(0.5f * 1.5707963f);
    const float unit = 0.25f * pan * (pan + (1.0f - pan));
    ASSERT_TRUE(isRamp(rec, 441 * 10, rampFrames, 0.5f * unit, 1.5f * unit, 2000));
    ASSERT_NEAR(r.e.getBusGain(bus), 1.5f, 1e-6f);
    PASS();
}

int main() { return runAllTests(); }
