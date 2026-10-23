#include "test_harness.h"
#include "broaudio/dsp/biquad.h"
#include <cmath>
#include <limits>
#include <numbers>

using namespace broaudio;

static constexpr int SR = 44100;
static constexpr float PI = std::numbers::pi_v<float>;

// Generate a sine wave, filter it, measure output amplitude
static float measureFilteredAmplitude(BiquadFilter& f, float freqHz, int sampleRate, int numCycles = 20) {
    int samplesPerCycle = sampleRate / static_cast<int>(freqHz);
    int totalSamples = samplesPerCycle * numCycles;
    float peakOut = 0.0f;

    for (int i = 0; i < totalSamples; i++) {
        float phase = static_cast<float>(i) / static_cast<float>(sampleRate);
        float input = std::sin(2.0f * PI * freqHz * phase);
        float output = f.process(input, 0);
        float absOut = std::fabs(output);
        // Only measure in the last few cycles (after settling)
        if (i > totalSamples / 2 && absOut > peakOut) peakOut = absOut;
    }
    return peakOut;
}

TEST(lowpass_passes_low_frequencies) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Lowpass;
    f.frequency = 5000.0f;
    f.Q = 0.707f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 100.0f, SR);
    ASSERT_GT(amp, 0.9f);
    PASS();
}

TEST(lowpass_attenuates_high_frequencies) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Lowpass;
    f.frequency = 1000.0f;
    f.Q = 0.707f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 10000.0f, SR);
    ASSERT_LT(amp, 0.1f);
    PASS();
}

TEST(highpass_passes_high_frequencies) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Highpass;
    f.frequency = 1000.0f;
    f.Q = 0.707f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 10000.0f, SR);
    ASSERT_GT(amp, 0.9f);
    PASS();
}

TEST(highpass_attenuates_low_frequencies) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Highpass;
    f.frequency = 5000.0f;
    f.Q = 0.707f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 100.0f, SR);
    ASSERT_LT(amp, 0.1f);
    PASS();
}

TEST(bandpass_passes_center_frequency) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Bandpass;
    f.frequency = 1000.0f;
    f.Q = 2.0f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 1000.0f, SR);
    ASSERT_GT(amp, 0.3f);
    PASS();
}

TEST(bandpass_attenuates_far_frequencies) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Bandpass;
    f.frequency = 1000.0f;
    f.Q = 2.0f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 10000.0f, SR);
    ASSERT_LT(amp, 0.15f);
    PASS();
}

TEST(notch_attenuates_center_frequency) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Notch;
    f.frequency = 1000.0f;
    f.Q = 5.0f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 1000.0f, SR);
    ASSERT_LT(amp, 0.15f);
    PASS();
}

TEST(notch_passes_distant_frequencies) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Notch;
    f.frequency = 1000.0f;
    f.Q = 5.0f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 5000.0f, SR);
    ASSERT_GT(amp, 0.8f);
    PASS();
}

TEST(allpass_preserves_amplitude) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Allpass;
    f.frequency = 1000.0f;
    f.Q = 0.707f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    float amp = measureFilteredAmplitude(f, 500.0f, SR);
    ASSERT_NEAR(amp, 1.0f, 0.05f);
    PASS();
}

TEST(filter_reset_clears_state) {
    BiquadFilter f;
    f.type = BiquadFilter::Type::Lowpass;
    f.frequency = 1000.0f;
    f.Q = 0.707f;
    f.computeCoefficients(SR);
    f.snapToTarget();

    // Run some signal through
    for (int i = 0; i < 100; i++) f.process(1.0f, 0);

    f.reset();
    ASSERT_NEAR(f.z1[0], 0.0f, 1e-10f);
    ASSERT_NEAR(f.z2[0], 0.0f, 1e-10f);
    PASS();
}

TEST(biquad_finite_float_gate_and_state_recovery) {
    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    const float infVal = std::numeric_limits<float>::infinity();
    const float badVals[] = {nanVal, infVal, -infVal, 0.0f, -500.0f, 1e9f};
    const BiquadFilter::Type types[] = {
        BiquadFilter::Type::Lowpass, BiquadFilter::Type::Highpass,
        BiquadFilter::Type::Bandpass, BiquadFilter::Type::Notch,
        BiquadFilter::Type::Allpass, BiquadFilter::Type::Peaking,
        BiquadFilter::Type::Lowshelf, BiquadFilter::Type::Highshelf
    };

    for (auto t : types) {
        for (float bf : badVals) {
            for (float bq : badVals) {
                for (float bg : badVals) {
                    BiquadFilter f;
                    f.type = t;
                    f.frequency = bf;
                    f.Q = bq;
                    f.gainDB = bg;
                    f.computeCoefficients(SR);
                    f.snapToTarget();
                    ASSERT_TRUE(std::isfinite(f.b0));
                    ASSERT_TRUE(std::isfinite(f.b1));
                    ASSERT_TRUE(std::isfinite(f.b2));
                    ASSERT_TRUE(std::isfinite(f.a1));
                    ASSERT_TRUE(std::isfinite(f.a2));

                    // Feed non-finite input followed by normal input: filter state must never stay poisoned
                    float yNan = f.process(nanVal, 0);
                    float yInf = f.process(infVal, 0);
                    ASSERT_TRUE(std::isfinite(yNan));
                    ASSERT_TRUE(std::isfinite(yInf));
                    for (int i = 0; i < 32; ++i) {
                        float y = f.process(0.5f, 0);
                        ASSERT_TRUE(std::isfinite(y));
                        ASSERT_TRUE(std::isfinite(f.z1[0]));
                        ASSERT_TRUE(std::isfinite(f.z2[0]));
                    }
                }
            }
        }
    }
    PASS();
}

int main() { return runAllTests(); }
