#include "test_harness.h"
#include "broaudio/synth/modulation.h"
#include <cmath>

using namespace broaudio;

static constexpr int SR = 44100;

TEST(lfo_sine_starts_at_offset) {
    ModMatrix mm;
    mm.setLfoShape(0, LfoShape::Sine);
    mm.setLfoRate(0, 1.0f);
    mm.setLfoDepth(0, 1.0f);
    mm.setLfoOffset(0, 0.0f);
    mm.setLfoBipolar(0, true);

    // Route LFO1 -> Pitch with amount 1.0
    mm.addRoute(ModSource::Lfo1, ModDest::Pitch, 1.0f);

    ModState state;
    state.reset(60, 1.0f);
    ModValues out;
    mm.process(out, state, 1.0f, SR);

    // At phase=0, sine=0, so pitch offset = 0
    ASSERT_NEAR(out.pitch, 0.0f, 0.01f);
    PASS();
}

TEST(lfo_square_alternates) {
    ModMatrix mm;
    mm.setLfoShape(0, LfoShape::Square);
    mm.setLfoRate(0, 1.0f);
    mm.setLfoDepth(0, 1.0f);
    mm.setLfoBipolar(0, true);

    mm.addRoute(ModSource::Lfo1, ModDest::Pitch, 1.0f);

    ModState state;
    state.reset(60, 1.0f);
    ModValues out;

    // First sample: phase starts at 0, square at phase<0.5 = +1
    mm.process(out, state, 1.0f, SR);
    ASSERT_NEAR(out.pitch, 1.0f, 0.01f);
    PASS();
}

TEST(velocity_source_scales_correctly) {
    ModMatrix mm;
    mm.addRoute(ModSource::Velocity, ModDest::Gain, 1.0f);

    ModState state;
    state.reset(60, 0.5f);  // velocity = 0.5
    ModValues out;
    mm.process(out, state, 1.0f, SR);

    // Gain = 1.0 * (1.0 + velocity * amount) = 1.0 * (1.0 + 0.5 * 1.0) = 1.5
    ASSERT_NEAR(out.gain, 1.5f, 0.01f);
    PASS();
}

TEST(key_tracking_maps_note_to_zero_one) {
    ModMatrix mm;
    mm.addRoute(ModSource::KeyTracking, ModDest::Pitch, 12.0f);

    ModState state;
    state.reset(64, 1.0f);  // note 64
    ModValues out;
    mm.process(out, state, 1.0f, SR);

    // KeyTracking = 64/127 ~= 0.504, pitch = 0.504 * 12 ~= 6.05
    float expected = (64.0f / 127.0f) * 12.0f;
    ASSERT_NEAR(out.pitch, expected, 0.1f);
    PASS();
}

TEST(envelope_source_passes_envelope) {
    ModMatrix mm;
    mm.addRoute(ModSource::Envelope, ModDest::Gain, -0.5f);

    ModState state;
    state.reset(60, 1.0f);
    ModValues out;
    mm.process(out, state, 0.8f, SR);  // envelope = 0.8

    // Gain = 1.0 * (1.0 + 0.8 * -0.5) = 1.0 * 0.6 = 0.6
    ASSERT_NEAR(out.gain, 0.6f, 0.01f);
    PASS();
}

TEST(disabled_route_has_no_effect) {
    ModMatrix mm;
    int idx = mm.addRoute(ModSource::Lfo1, ModDest::Pitch, 100.0f);
    mm.setRouteEnabled(idx, false);

    ModState state;
    state.reset(60, 1.0f);
    ModValues out;
    mm.process(out, state, 1.0f, SR);

    ASSERT_NEAR(out.pitch, 0.0f, 0.01f);
    PASS();
}

TEST(multiple_routes_to_same_dest_accumulate) {
    ModMatrix mm;
    mm.setLfoShape(0, LfoShape::Square);
    mm.setLfoRate(0, 1.0f);
    mm.setLfoDepth(0, 1.0f);
    mm.setLfoBipolar(0, true);

    mm.addRoute(ModSource::Lfo1, ModDest::Pitch, 2.0f);
    mm.addRoute(ModSource::Lfo1, ModDest::Pitch, 3.0f);

    ModState state;
    state.reset(60, 1.0f);
    ModValues out;
    mm.process(out, state, 1.0f, SR);

    // Square at phase 0 = +1, two routes sum: 1*2 + 1*3 = 5
    ASSERT_NEAR(out.pitch, 5.0f, 0.01f);
    PASS();
}

TEST(filter_freq_mod_uses_exp2) {
    ModMatrix mm;
    mm.addRoute(ModSource::Velocity, ModDest::FilterFreq, 1.0f);

    ModState state;
    state.reset(60, 1.0f);  // velocity = 1.0
    ModValues out;
    mm.process(out, state, 1.0f, SR);

    // filterFreq = 1.0 * exp2(1.0 * 1.0) = exp2(1) = 2.0
    ASSERT_NEAR(out.filterFreq, 2.0f, 0.01f);
    PASS();
}

TEST(remove_route_works) {
    ModMatrix mm;
    mm.addRoute(ModSource::Velocity, ModDest::Pitch, 5.0f);
    int idx = mm.addRoute(ModSource::Velocity, ModDest::Pitch, 10.0f);
    ASSERT_EQ(mm.routeCount(), 2);

    mm.removeRoute(0);
    ASSERT_EQ(mm.routeCount(), 1);

    // The remaining route should be the second one (amount=10)
    ModState state;
    state.reset(60, 1.0f);
    ModValues out;
    mm.process(out, state, 1.0f, SR);
    ASSERT_NEAR(out.pitch, 10.0f, 0.01f);
    PASS();
}

TEST(mod_wheel_source) {
    ModMatrix mm;
    mm.setModWheel(0.75f);
    mm.addRoute(ModSource::ModWheel, ModDest::Pitch, 2.0f);

    ModState state;
    state.reset(60, 1.0f);
    ModValues out;
    mm.process(out, state, 1.0f, SR);

    ASSERT_NEAR(out.pitch, 1.5f, 0.01f);
    PASS();
}

TEST(unipolar_lfo_outputs_zero_to_one) {
    ModMatrix mm;
    mm.setLfoShape(0, LfoShape::Square);
    mm.setLfoRate(0, 1.0f);
    mm.setLfoDepth(0, 1.0f);
    mm.setLfoBipolar(0, false);  // unipolar

    mm.addRoute(ModSource::Lfo1, ModDest::Pitch, 1.0f);

    ModState state;
    state.reset(60, 1.0f);
    ModValues out;

    // Square at phase 0, unipolar: (1+1)*0.5 = 1.0
    mm.process(out, state, 1.0f, SR);
    ASSERT_NEAR(out.pitch, 1.0f, 0.01f);
    PASS();
}

int main() { return runAllTests(); }
