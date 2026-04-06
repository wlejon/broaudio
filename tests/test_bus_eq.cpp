#include "test_harness.h"
#include "broaudio/dsp/equalizer.h"
#include "broaudio/dsp/params.h"
#include "broaudio/mix/bus.h"
#include "broaudio/types.h"

#include <cmath>
#include <vector>

using namespace broaudio;

// --- EqualizerParams ---

TEST(eq_params_default_disabled) {
    EqualizerParams p;
    ASSERT_FALSE(p.enabled.load());
    ASSERT_NEAR(p.masterGain.load(), 0.0f, 0.001f);
    for (int i = 0; i < 7; i++)
        ASSERT_NEAR(p.bandGains[i].load(), 0.0f, 0.001f);
    PASS();
}

TEST(eq_params_version_increments) {
    EqualizerParams p;
    uint32_t v0 = p.version.load();
    p.version.fetch_add(1);
    ASSERT_EQ(p.version.load(), v0 + 1);
    PASS();
}

// --- Equalizer processStereoInterleaved ---

TEST(eq_interleaved_passthrough_when_flat) {
    Equalizer eq(44100);
    eq.setEnabled(true);
    eq.setMasterGain(0.0f); // 0 dB = unity

    // Generate a 440 Hz sine test signal (stereo interleaved)
    const int frames = 512;
    std::vector<float> buf(frames * 2);
    std::vector<float> original(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * 3.14159265f * 440.0f * i / 44100.0f) * 0.5f;
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
        original[i * 2] = s;
        original[i * 2 + 1] = s;
    }

    eq.processStereoInterleaved(buf.data(), frames);

    // With all bands at 0 dB and master at 0 dB, output should be close to input
    float maxDiff = 0.0f;
    for (int i = 0; i < frames * 2; i++) {
        float d = std::fabs(buf[i] - original[i]);
        if (d > maxDiff) maxDiff = d;
    }
    ASSERT_LT(maxDiff, 0.01f);
    PASS();
}

TEST(eq_interleaved_boosts_signal) {
    Equalizer eq(44100);
    eq.setEnabled(true);
    eq.setMasterGain(6.0f); // +6 dB

    const int frames = 256;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = 0.1f;
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    eq.processStereoInterleaved(buf.data(), frames);

    // +6 dB ≈ 2x gain, so 0.1 → ~0.2
    ASSERT_GT(std::fabs(buf[frames]), 0.15f);
    PASS();
}

TEST(eq_disabled_no_change) {
    Equalizer eq(44100);
    eq.setEnabled(false);

    const int frames = 128;
    std::vector<float> buf(frames * 2, 0.5f);
    eq.processStereoInterleaved(buf.data(), frames);

    // Should be unchanged
    ASSERT_NEAR(buf[0], 0.5f, 0.0001f);
    ASSERT_NEAR(buf[1], 0.5f, 0.0001f);
    PASS();
}

// --- EffectSlot includes Equalizer ---

TEST(effect_slot_equalizer_exists) {
    ASSERT_EQ(static_cast<int>(EffectSlot::Equalizer), 5);
    ASSERT_EQ(static_cast<int>(EffectSlot::Count), 7);
    PASS();
}

TEST(bus_default_order_includes_eq) {
    Bus bus;
    ASSERT_EQ(bus.effectOrder[5].load(), static_cast<uint8_t>(EffectSlot::Equalizer));
    ASSERT_EQ(Bus::NUM_EFFECT_SLOTS, 7);
    PASS();
}

// --- Per-band frequency response verification ---

static constexpr int TEST_SR = 44100;

// Helper: measure amplitude of a sine at a given frequency after EQ processing
static float measureEqResponse(Equalizer& eq, float freq) {
    const int frames = 4096;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * 3.14159265f * freq * i / TEST_SR) * 0.5f;
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
    eq.reset();
    eq.processStereoInterleaved(buf.data(), frames);

    // Measure peak amplitude in the second half (after filter settles)
    float peak = 0.0f;
    for (int i = frames / 2; i < frames; i++) {
        float a = std::fabs(buf[i * 2]);
        if (a > peak) peak = a;
    }
    return peak;
}

TEST(eq_band_boost_increases_amplitude_at_center_freq) {
    // Boost each band by +12dB and verify the signal at that frequency gets louder
    for (int band = 0; band < Equalizer::NUM_BANDS; band++) {
        float centerFreq = Equalizer::BAND_FREQUENCIES[band];
        if (centerFreq > TEST_SR / 2.5f) continue;  // skip near-Nyquist

        // Flat EQ reference
        Equalizer eqFlat(TEST_SR);
        eqFlat.setEnabled(true);
        float ampFlat = measureEqResponse(eqFlat, centerFreq);

        // Boosted band
        Equalizer eqBoosted(TEST_SR);
        eqBoosted.setEnabled(true);
        eqBoosted.setBandGain(band, 12.0f);
        float ampBoosted = measureEqResponse(eqBoosted, centerFreq);

        // +12dB should roughly quadruple amplitude (4x). Require at least 2x.
        ASSERT_GT(ampBoosted, ampFlat * 2.0f);
    }
    PASS();
}

TEST(eq_band_cut_decreases_amplitude_at_center_freq) {
    for (int band = 0; band < Equalizer::NUM_BANDS; band++) {
        float centerFreq = Equalizer::BAND_FREQUENCIES[band];
        if (centerFreq > TEST_SR / 2.5f) continue;

        Equalizer eqFlat(TEST_SR);
        eqFlat.setEnabled(true);
        float ampFlat = measureEqResponse(eqFlat, centerFreq);

        Equalizer eqCut(TEST_SR);
        eqCut.setEnabled(true);
        eqCut.setBandGain(band, -12.0f);
        float ampCut = measureEqResponse(eqCut, centerFreq);

        // -12dB should reduce to roughly 1/4. Require at least 50% reduction.
        ASSERT_LT(ampCut, ampFlat * 0.5f);
    }
    PASS();
}

TEST(eq_boost_is_localized_to_target_band) {
    // Boosting 1kHz (band 3) should not significantly affect 60Hz (band 0)
    Equalizer eq(TEST_SR);
    eq.setEnabled(true);
    eq.setBandGain(3, 12.0f);  // boost 1kHz

    float ampAt60 = measureEqResponse(eq, 60.0f);

    Equalizer eqFlat(TEST_SR);
    eqFlat.setEnabled(true);
    float ampAt60Flat = measureEqResponse(eqFlat, 60.0f);

    // 60Hz should be within ±3dB of flat response
    float ratio = ampAt60 / (ampAt60Flat + 1e-10f);
    ASSERT_GT(ratio, 0.7f);   // not more than ~3dB down
    ASSERT_LT(ratio, 1.41f);  // not more than ~3dB up
    PASS();
}

TEST(eq_negative_master_gain_reduces_volume) {
    Equalizer eqFlat(TEST_SR);
    eqFlat.setEnabled(true);
    eqFlat.setMasterGain(0.0f);
    float ampFlat = measureEqResponse(eqFlat, 440.0f);

    Equalizer eqCut(TEST_SR);
    eqCut.setEnabled(true);
    eqCut.setMasterGain(-6.0f);
    float ampCut = measureEqResponse(eqCut, 440.0f);

    // -6dB should halve amplitude (±tolerance)
    ASSERT_GT(ampFlat, 0.01f);  // sanity: flat signal is non-zero
    ASSERT_LT(ampCut, ampFlat * 0.65f);  // at least ~35% reduction
    ASSERT_GT(ampCut, ampFlat * 0.35f);  // not more than ~65% reduction
    PASS();
}

TEST(eq_master_gain_applies_uniformly) {
    // +6dB master gain should double amplitude at all frequencies
    Equalizer eqFlat(TEST_SR);
    eqFlat.setEnabled(true);
    eqFlat.setMasterGain(0.0f);
    float amp440Flat = measureEqResponse(eqFlat, 440.0f);
    float amp2kFlat = measureEqResponse(eqFlat, 2000.0f);

    Equalizer eqGain(TEST_SR);
    eqGain.setEnabled(true);
    eqGain.setMasterGain(6.0f);
    float amp440Gain = measureEqResponse(eqGain, 440.0f);
    float amp2kGain = measureEqResponse(eqGain, 2000.0f);

    // Both should roughly double (±1dB tolerance)
    ASSERT_NEAR(amp440Gain / amp440Flat, 2.0f, 0.3f);
    ASSERT_NEAR(amp2kGain / amp2kFlat, 2.0f, 0.3f);
    PASS();
}

int main() { return runAllTests(); }
