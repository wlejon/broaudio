#include "test_harness.h"
#include "broaudio/synth/oscillator.h"
#include "broaudio/dsp/fft.h"
#include <cmath>
#include <numbers>
#include <vector>

using namespace broaudio;

// Helper: generate N samples of a waveform at a given frequency
static std::vector<float> generateWaveform(Waveform wf, float freq, int sampleRate, int numSamples) {
    std::vector<float> buf(numSamples);
    float phase = 0.0f;
    float phaseInc = freq / sampleRate;
    for (int i = 0; i < numSamples; i++) {
        buf[i] = generateSample(wf, phase, phaseInc);
        phase += phaseInc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    return buf;
}

// Helper: compute magnitude spectrum (first half = N/2 bins)
static std::vector<float> spectrum(const float* samples, int n) {
    std::vector<float> re(n), im(n, 0.0f);
    // Apply Hann window
    for (int i = 0; i < n; i++) {
        float w = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * i / n));
        re[i] = samples[i] * w;
    }
    fft(re.data(), im.data(), n);
    int bins = n / 2;
    std::vector<float> mag(bins);
    float invN = 1.0f / n;
    for (int i = 0; i < bins; i++) {
        mag[i] = std::sqrt(re[i] * re[i] + im[i] * im[i]) * invN;
    }
    return mag;
}

// Helper: find peak bin index
static int peakBin(const std::vector<float>& mag) {
    int best = 0;
    for (int i = 1; i < static_cast<int>(mag.size()); i++) {
        if (mag[i] > mag[best]) best = i;
    }
    return best;
}

// --- Sine ---

TEST(sine_at_zero_phase) {
    float s = generateSample(Waveform::Sine, 0.0f, 0.01f);
    ASSERT_NEAR(s, 0.0f, 1e-5f);
    PASS();
}

TEST(sine_at_quarter_phase) {
    float s = generateSample(Waveform::Sine, 0.25f, 0.01f);
    ASSERT_NEAR(s, 1.0f, 1e-5f);
    PASS();
}

TEST(sine_at_half_phase) {
    float s = generateSample(Waveform::Sine, 0.5f, 0.01f);
    ASSERT_NEAR(s, 0.0f, 1e-5f);
    PASS();
}

TEST(sine_at_three_quarter_phase) {
    float s = generateSample(Waveform::Sine, 0.75f, 0.01f);
    ASSERT_NEAR(s, -1.0f, 1e-5f);
    PASS();
}

// --- Square ---

TEST(square_first_half_positive) {
    // Well away from transitions where polyBLEP acts
    float s = generateSample(Waveform::Square, 0.25f, 0.001f);
    ASSERT_NEAR(s, 1.0f, 0.01f);
    PASS();
}

TEST(square_second_half_negative) {
    float s = generateSample(Waveform::Square, 0.75f, 0.001f);
    ASSERT_NEAR(s, -1.0f, 0.01f);
    PASS();
}

// --- Sawtooth ---

TEST(saw_midpoint) {
    // At phase=0.5, naive saw = 2*0.5-1 = 0. polyBLEP is negligible away from edges.
    float s = generateSample(Waveform::Sawtooth, 0.5f, 0.001f);
    ASSERT_NEAR(s, 0.0f, 0.01f);
    PASS();
}

TEST(saw_range) {
    // Generate a full cycle and check it spans roughly -1 to 1
    float minV = 1.0f, maxV = -1.0f;
    for (int i = 0; i < 1000; i++) {
        float phase = static_cast<float>(i) / 1000.0f;
        float s = generateSample(Waveform::Sawtooth, phase, 0.001f);
        if (s < minV) minV = s;
        if (s > maxV) maxV = s;
    }
    ASSERT_LT(minV, -0.9f);
    ASSERT_GT(maxV, 0.9f);
    PASS();
}

// --- Triangle ---

TEST(triangle_rising_first_half) {
    // Triangle: (phase < 0.5) ? 4*phase-1 : 3-4*phase
    // At phase=0.1, value should be 4*0.1-1 = -0.6
    // At phase=0.4, value should be 4*0.4-1 = 0.6
    float lo = generateSample(Waveform::Triangle, 0.1f, 0.001f);
    float hi = generateSample(Waveform::Triangle, 0.4f, 0.001f);
    ASSERT_GT(hi, lo);
    ASSERT_NEAR(lo, -0.6f, 0.1f);
    ASSERT_NEAR(hi, 0.6f, 0.1f);
    PASS();
}

TEST(triangle_falling_second_half) {
    // At phase=0.6, value should be 3-4*0.6 = 0.6
    // At phase=0.9, value should be 3-4*0.9 = -0.6
    float hi = generateSample(Waveform::Triangle, 0.6f, 0.001f);
    float lo = generateSample(Waveform::Triangle, 0.9f, 0.001f);
    ASSERT_GT(hi, lo);
    ASSERT_NEAR(hi, 0.6f, 0.1f);
    ASSERT_NEAR(lo, -0.6f, 0.1f);
    PASS();
}

// --- Noise ---

TEST(white_noise_range) {
    NoiseState state{};
    float minV = 1.0f, maxV = -1.0f;
    for (int i = 0; i < 10000; i++) {
        float s = generateNoise(Waveform::WhiteNoise, state);
        if (s < minV) minV = s;
        if (s > maxV) maxV = s;
    }
    ASSERT_LT(minV, -0.5f);
    ASSERT_GT(maxV, 0.5f);
    PASS();
}

TEST(white_noise_mean_near_zero) {
    NoiseState state{};
    double sum = 0.0;
    int n = 100000;
    for (int i = 0; i < n; i++) {
        sum += generateNoise(Waveform::WhiteNoise, state);
    }
    float mean = static_cast<float>(sum / n);
    ASSERT_NEAR(mean, 0.0f, 0.05f);
    PASS();
}

TEST(pink_noise_bounded) {
    NoiseState state{};
    for (int i = 0; i < 10000; i++) {
        float s = generateNoise(Waveform::PinkNoise, state);
        ASSERT_TRUE(s >= -2.0f && s <= 2.0f);
    }
    PASS();
}

TEST(brown_noise_bounded) {
    NoiseState state{};
    for (int i = 0; i < 100000; i++) {
        float s = generateNoise(Waveform::BrownNoise, state);
        ASSERT_TRUE(s >= -1.0f && s <= 1.0f);
    }
    PASS();
}

// --- Pan gains ---

TEST(pan_center_equal) {
    float L, R;
    panGains(0.0f, L, R);
    ASSERT_NEAR(L, R, 0.001f);
    // Equal power: each should be ~0.707
    ASSERT_NEAR(L, 0.707f, 0.01f);
    PASS();
}

TEST(pan_hard_left) {
    float L, R;
    panGains(-1.0f, L, R);
    ASSERT_NEAR(L, 1.0f, 0.001f);
    ASSERT_NEAR(R, 0.0f, 0.001f);
    PASS();
}

TEST(pan_hard_right) {
    float L, R;
    panGains(1.0f, L, R);
    ASSERT_NEAR(L, 0.0f, 0.001f);
    ASSERT_NEAR(R, 1.0f, 0.001f);
    PASS();
}

// --- FFT spectral verification ---

static constexpr int SR = 44100;
static constexpr int FFT_N = 4096;

TEST(sine_440_fundamental_at_correct_bin) {
    // 440Hz sine: peak should be at bin 440*N/SR = 440*4096/44100 ≈ 40.8 → bin 41
    auto buf = generateWaveform(Waveform::Sine, 440.0f, SR, FFT_N);
    auto mag = spectrum(buf.data(), FFT_N);

    int peak = peakBin(mag);
    float peakFreq = peak * static_cast<float>(SR) / FFT_N;
    // Within ±1 bin = ±10.7Hz
    ASSERT_GT(peakFreq, 420.0f);
    ASSERT_LT(peakFreq, 460.0f);
    PASS();
}

TEST(sine_has_single_dominant_partial) {
    // Sine should have energy only at the fundamental — no harmonics
    auto buf = generateWaveform(Waveform::Sine, 440.0f, SR, FFT_N);
    auto mag = spectrum(buf.data(), FFT_N);

    int peak = peakBin(mag);
    float peakMag = mag[peak];

    // Count bins with significant energy (> 1% of peak)
    int significantBins = 0;
    for (int i = 1; i < static_cast<int>(mag.size()); i++) {
        if (mag[i] > peakMag * 0.01f) significantBins++;
    }
    // Sine + Hann window spreads across ~5 bins, so allow up to 8
    ASSERT_LT(significantBins, 8);
    PASS();
}

TEST(saw_has_harmonics_at_integer_multiples) {
    // Sawtooth at 440Hz should have harmonics at 880, 1320, 1760, etc.
    auto buf = generateWaveform(Waveform::Sawtooth, 440.0f, SR, FFT_N);
    auto mag = spectrum(buf.data(), FFT_N);

    float binHz = static_cast<float>(SR) / FFT_N;
    int fundBin = static_cast<int>(440.0f / binHz + 0.5f);
    float fundMag = mag[fundBin];

    // Check harmonics 2-5 are present (>5% of fundamental)
    for (int h = 2; h <= 5; h++) {
        int hBin = static_cast<int>(440.0f * h / binHz + 0.5f);
        if (hBin >= static_cast<int>(mag.size())) break;
        ASSERT_GT(mag[hBin], fundMag * 0.05f);
    }
    PASS();
}

TEST(saw_harmonics_decay_as_1_over_n) {
    // Sawtooth harmonic n should have amplitude ~1/n relative to fundamental
    auto buf = generateWaveform(Waveform::Sawtooth, 200.0f, SR, FFT_N);
    auto mag = spectrum(buf.data(), FFT_N);

    float binHz = static_cast<float>(SR) / FFT_N;
    int fundBin = static_cast<int>(200.0f / binHz + 0.5f);
    float fundMag = mag[fundBin];

    // Check harmonics 2-8: amplitude should be roughly fundMag/n
    // Allow generous tolerance (0.3x-3x) for windowing + polyBLEP effects
    for (int n = 2; n <= 8; n++) {
        int hBin = static_cast<int>(200.0f * n / binHz + 0.5f);
        if (hBin >= static_cast<int>(mag.size())) break;
        float expected = fundMag / n;
        ASSERT_GT(mag[hBin], expected * 0.3f);
        ASSERT_LT(mag[hBin], expected * 3.0f);
    }
    PASS();
}

TEST(square_has_only_odd_harmonics) {
    // Square wave should have harmonics at 1f, 3f, 5f, 7f (odd only)
    // Even harmonics (2f, 4f, 6f) should be near zero
    auto buf = generateWaveform(Waveform::Square, 200.0f, SR, FFT_N);
    auto mag = spectrum(buf.data(), FFT_N);

    float binHz = static_cast<float>(SR) / FFT_N;
    int fundBin = static_cast<int>(200.0f / binHz + 0.5f);
    float fundMag = mag[fundBin];

    // Odd harmonics 3, 5, 7 should be present (>5% of fundamental)
    for (int n : {3, 5, 7}) {
        int hBin = static_cast<int>(200.0f * n / binHz + 0.5f);
        if (hBin >= static_cast<int>(mag.size())) break;
        ASSERT_GT(mag[hBin], fundMag * 0.05f);
    }

    // Even harmonics 2, 4, 6 should be very weak (<5% of fundamental)
    for (int n : {2, 4, 6}) {
        int hBin = static_cast<int>(200.0f * n / binHz + 0.5f);
        if (hBin >= static_cast<int>(mag.size())) break;
        ASSERT_LT(mag[hBin], fundMag * 0.05f);
    }
    PASS();
}

TEST(triangle_fundamental_at_correct_frequency) {
    auto buf = generateWaveform(Waveform::Triangle, 1000.0f, SR, FFT_N);
    auto mag = spectrum(buf.data(), FFT_N);

    int peak = peakBin(mag);
    float peakFreq = peak * static_cast<float>(SR) / FFT_N;
    ASSERT_GT(peakFreq, 980.0f);
    ASSERT_LT(peakFreq, 1020.0f);
    PASS();
}

// --- Noise spectral characterization ---

TEST(white_noise_flat_spectrum) {
    // White noise should have roughly equal energy across octave bands
    NoiseState state{};
    std::vector<float> buf(FFT_N);
    for (int i = 0; i < FFT_N; i++) buf[i] = generateNoise(Waveform::WhiteNoise, state);

    auto mag = spectrum(buf.data(), FFT_N);

    // Compare average energy in low band (200-400Hz) vs high band (4000-8000Hz)
    float binHz = static_cast<float>(SR) / FFT_N;
    float lowEnergy = 0.0f, highEnergy = 0.0f;
    int lowCount = 0, highCount = 0;
    for (int i = 0; i < static_cast<int>(mag.size()); i++) {
        float freq = i * binHz;
        if (freq >= 200.0f && freq < 400.0f) { lowEnergy += mag[i] * mag[i]; lowCount++; }
        if (freq >= 4000.0f && freq < 8000.0f) { highEnergy += mag[i] * mag[i]; highCount++; }
    }
    lowEnergy /= lowCount;
    highEnergy /= highCount;

    // For white noise, bands should have similar energy density (within 6dB)
    float ratio = lowEnergy / (highEnergy + 1e-20f);
    ASSERT_GT(ratio, 0.25f);  // high band not more than 6dB above low
    ASSERT_LT(ratio, 4.0f);   // low band not more than 6dB above high
    PASS();
}

TEST(pink_noise_has_falling_spectrum) {
    // Pink noise: power density falls ~3dB per octave (1/f)
    // So a higher band should have measurably less energy density than a lower band
    NoiseState state{};
    // Average over multiple FFT windows for stability
    int numWindows = 16;
    std::vector<float> avgMag(FFT_N / 2, 0.0f);
    for (int w = 0; w < numWindows; w++) {
        std::vector<float> buf(FFT_N);
        for (int i = 0; i < FFT_N; i++) buf[i] = generateNoise(Waveform::PinkNoise, state);
        auto mag = spectrum(buf.data(), FFT_N);
        for (int i = 0; i < FFT_N / 2; i++) avgMag[i] += mag[i] * mag[i];
    }
    for (int i = 0; i < FFT_N / 2; i++) avgMag[i] /= numWindows;

    float binHz = static_cast<float>(SR) / FFT_N;

    // Measure average power density in octave bands: 200-400Hz vs 3200-6400Hz (4 octaves apart)
    float lowPower = 0.0f, highPower = 0.0f;
    int lowCount = 0, highCount = 0;
    for (int i = 0; i < FFT_N / 2; i++) {
        float freq = i * binHz;
        if (freq >= 200.0f && freq < 400.0f) { lowPower += avgMag[i]; lowCount++; }
        if (freq >= 3200.0f && freq < 6400.0f) { highPower += avgMag[i]; highCount++; }
    }
    lowPower /= lowCount;
    highPower /= highCount;

    // 4 octaves apart at -3dB/octave = -12dB power = 1/16 ratio
    // Be generous: just verify high band is at least 3dB down (ratio < 0.5)
    float ratio = highPower / (lowPower + 1e-20f);
    ASSERT_LT(ratio, 0.5f);
    PASS();
}

int main() { return runAllTests(); }
