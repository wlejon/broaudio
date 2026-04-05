#include "test_harness.h"
#include "broaudio/synth/wavetable.h"
#include <cmath>
#include <numbers>

using namespace broaudio;

static constexpr int SR = 44100;
static constexpr float PI = std::numbers::pi_v<float>;

TEST(saw_wavetable_creates_levels) {
    auto bank = WavetableBank::createSaw(SR);
    ASSERT_EQ(bank->numLevels(), WavetableBank::MAX_LEVELS);
    PASS();
}

TEST(square_wavetable_creates_levels) {
    auto bank = WavetableBank::createSquare(SR);
    ASSERT_EQ(bank->numLevels(), WavetableBank::MAX_LEVELS);
    PASS();
}

TEST(triangle_wavetable_creates_levels) {
    auto bank = WavetableBank::createTriangle(SR);
    ASSERT_EQ(bank->numLevels(), WavetableBank::MAX_LEVELS);
    PASS();
}

TEST(saw_wavetable_samples_are_bounded) {
    auto bank = WavetableBank::createSaw(SR);
    for (int i = 0; i < 2048; i++) {
        float phase = static_cast<float>(i) / 2048.0f;
        float s = bank->sample(phase, 440.0f / SR);
        ASSERT_TRUE(s >= -1.5f && s <= 1.5f);
    }
    PASS();
}

TEST(wavetable_zero_phase_inc_returns_value) {
    auto bank = WavetableBank::createSaw(SR);
    // Should not crash, and should return some finite value
    float s = bank->sample(0.5f, 0.0f);
    ASSERT_TRUE(std::isfinite(s));
    PASS();
}

TEST(custom_wavetable_from_sine) {
    // Create a single sine cycle as input
    float waveform[WavetableBank::TABLE_SIZE];
    for (int i = 0; i < WavetableBank::TABLE_SIZE; i++) {
        float phase = static_cast<float>(i) / WavetableBank::TABLE_SIZE;
        waveform[i] = std::sin(2.0f * PI * phase);
    }

    auto bank = WavetableBank::createFromWaveform(waveform, WavetableBank::TABLE_SIZE, SR);
    ASSERT_EQ(bank->numLevels(), WavetableBank::MAX_LEVELS);

    // Sample at phase=0.25 should be close to sin(pi/2) = 1.0
    float s = bank->sample(0.25f, 440.0f / SR);
    ASSERT_NEAR(s, 1.0f, 0.05f);
    PASS();
}

TEST(saw_wavetable_has_expected_range) {
    auto bank = WavetableBank::createSaw(SR);
    float phaseInc = 100.0f / SR;

    // The additive saw spans a useful range across one period
    float minV = 2.0f, maxV = -2.0f;
    for (int i = 0; i < 2048; i++) {
        float phase = static_cast<float>(i) / 2048.0f;
        float v = bank->sample(phase, phaseInc);
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
    }
    // Should span roughly -1 to +1
    ASSERT_GT(maxV, 0.8f);
    ASSERT_LT(minV, -0.8f);
    // Antisymmetric: value at 0 and 0.5 should be near zero
    float atZero = bank->sample(0.0f, phaseInc);
    float atHalf = bank->sample(0.5f, phaseInc);
    ASSERT_NEAR(atZero, 0.0f, 0.15f);
    ASSERT_NEAR(atHalf, 0.0f, 0.15f);
    PASS();
}

TEST(triangle_wavetable_peaks) {
    auto bank = WavetableBank::createTriangle(SR);
    float phaseInc = 100.0f / SR;

    // Triangle peak at phase=0.25
    float peak = bank->sample(0.25f, phaseInc);
    ASSERT_GT(peak, 0.8f);

    // Triangle trough at phase=0.75
    float trough = bank->sample(0.75f, phaseInc);
    ASSERT_LT(trough, -0.8f);
    PASS();
}

TEST(empty_bank_returns_zero) {
    WavetableBank bank;
    float s = bank.sample(0.5f, 0.01f);
    ASSERT_NEAR(s, 0.0f, 1e-10f);
    PASS();
}

int main() { return runAllTests(); }
