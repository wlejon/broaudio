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
    ASSERT_EQ(static_cast<int>(EffectSlot::Count), 6);
    PASS();
}

TEST(bus_default_order_includes_eq) {
    Bus bus;
    ASSERT_EQ(bus.effectOrder[5].load(), static_cast<uint8_t>(EffectSlot::Equalizer));
    ASSERT_EQ(Bus::NUM_EFFECT_SLOTS, 6);
    PASS();
}

int main() { return runAllTests(); }
