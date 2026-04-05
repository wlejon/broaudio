#include "test_harness.h"
#include "broaudio/dsp/fft.h"
#include <cmath>
#include <numbers>
#include <vector>

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;

TEST(fft_dc_signal) {
    const int N = 16;
    float real[N], imag[N];
    for (int i = 0; i < N; i++) { real[i] = 1.0f; imag[i] = 0.0f; }

    fft(real, imag, N);

    // Bin 0 (DC) should be N, all others ~0
    ASSERT_NEAR(real[0], static_cast<float>(N), 1e-3f);
    for (int i = 1; i < N; i++) {
        ASSERT_NEAR(real[i], 0.0f, 1e-3f);
        ASSERT_NEAR(imag[i], 0.0f, 1e-3f);
    }
    PASS();
}

TEST(fft_single_sine) {
    const int N = 256;
    std::vector<float> real(N), imag(N, 0.0f);

    // Single sine at bin k=4
    int k = 4;
    for (int i = 0; i < N; i++) {
        real[i] = std::sin(2.0f * PI * k * static_cast<float>(i) / N);
    }

    fft(real.data(), imag.data(), N);

    // Magnitude at bin k should be N/2, others negligible
    float magK = std::sqrt(real[k] * real[k] + imag[k] * imag[k]);
    ASSERT_NEAR(magK, N / 2.0f, 0.5f);

    // Check that other bins (except mirror at N-k) are near zero
    for (int i = 1; i < N / 2; i++) {
        if (i == k) continue;
        float mag = std::sqrt(real[i] * real[i] + imag[i] * imag[i]);
        ASSERT_LT(mag, 1.0f);
    }
    PASS();
}

TEST(fft_cosine_goes_to_real_part) {
    const int N = 128;
    std::vector<float> real(N), imag(N, 0.0f);

    int k = 8;
    for (int i = 0; i < N; i++) {
        real[i] = std::cos(2.0f * PI * k * static_cast<float>(i) / N);
    }

    fft(real.data(), imag.data(), N);

    // Cosine: real part at bin k should be N/2, imaginary ~0
    ASSERT_NEAR(real[k], N / 2.0f, 0.5f);
    ASSERT_NEAR(imag[k], 0.0f, 0.5f);
    PASS();
}

TEST(fft_zero_input) {
    const int N = 64;
    float real[64] = {}, imag[64] = {};

    fft(real, imag, N);

    for (int i = 0; i < N; i++) {
        ASSERT_NEAR(real[i], 0.0f, 1e-6f);
        ASSERT_NEAR(imag[i], 0.0f, 1e-6f);
    }
    PASS();
}

TEST(fft_impulse) {
    const int N = 32;
    float real[32] = {}, imag[32] = {};
    real[0] = 1.0f;  // unit impulse

    fft(real, imag, N);

    // FFT of impulse = flat spectrum (all bins = 1)
    for (int i = 0; i < N; i++) {
        ASSERT_NEAR(real[i], 1.0f, 1e-5f);
        ASSERT_NEAR(imag[i], 0.0f, 1e-5f);
    }
    PASS();
}

int main() { return runAllTests(); }
