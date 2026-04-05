#include "test_harness.h"
#include "broaudio/dsp/limiter.h"
#include <cmath>
#include <vector>

using namespace broaudio;

static constexpr int SR = 44100;

TEST(limiter_transparent_below_threshold) {
    Limiter lim(SR, 1);
    lim.setThreshold(-6.0f);

    // Signal at -20 dB (~0.1 amplitude)
    int n = 5000;
    std::vector<float> buf(n, 0.1f);
    lim.process(buf.data(), n);

    // Should pass through approximately unchanged (accounting for lookahead delay)
    // Check the tail end after the delay line has flushed
    float last = buf[n - 1];
    ASSERT_NEAR(last, 0.1f, 0.02f);
    PASS();
}

TEST(limiter_reduces_loud_signal) {
    Limiter lim(SR, 1);
    lim.setThreshold(-6.0f);  // ~0.5 amplitude

    // Signal at 0 dB (amplitude 1.0) — well above threshold
    int n = 10000;
    std::vector<float> buf(n, 1.0f);
    lim.process(buf.data(), n);

    // Output should be limited to threshold (~0.5)
    float peak = 0.0f;
    for (int i = n / 2; i < n; i++) {
        if (std::fabs(buf[i]) > peak) peak = std::fabs(buf[i]);
    }
    ASSERT_LT(peak, 0.6f);
    PASS();
}

TEST(limiter_brickwall_never_exceeds_threshold) {
    Limiter lim(SR, 2);
    lim.setThreshold(-3.0f);  // ~0.708 amplitude

    float threshLin = std::pow(10.0f, -3.0f / 20.0f);

    // Feed a very hot signal: +12 dB (amplitude 4.0)
    int frames = 10000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames * 2; i++) buf[i] = 4.0f;

    lim.process(buf.data(), frames);

    // After the lookahead settles, output must never exceed threshold.
    // Allow a tiny margin for the first few samples during lookahead fill.
    float peak = 0.0f;
    for (int i = frames / 2; i < frames; i++) {
        float a = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
        if (a > peak) peak = a;
    }
    ASSERT_LT(peak, threshLin + 0.01f);
    PASS();
}

TEST(limiter_disabled_passes_through) {
    Limiter lim(SR, 1);
    lim.setEnabled(false);

    int n = 1000;
    std::vector<float> buf(n, 0.95f);
    lim.process(buf.data(), n);

    // Disabled limiter should not touch the signal
    ASSERT_NEAR(buf[500], 0.95f, 1e-6f);
    PASS();
}

TEST(limiter_stereo_processes_both_channels) {
    Limiter lim(SR, 2);
    lim.setThreshold(-6.0f);

    int frames = 5000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames * 2; i++) buf[i] = 0.95f;

    lim.process(buf.data(), frames);

    // Both channels should be limited
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = frames / 2; i < frames; i++) {
        if (std::fabs(buf[i * 2]) > peakL) peakL = std::fabs(buf[i * 2]);
        if (std::fabs(buf[i * 2 + 1]) > peakR) peakR = std::fabs(buf[i * 2 + 1]);
    }
    float thresh = std::pow(10.0f, -6.0f / 20.0f);
    ASSERT_LT(peakL, thresh + 0.01f);
    ASSERT_LT(peakR, thresh + 0.01f);
    PASS();
}

TEST(limiter_reset_clears_envelope) {
    Limiter lim(SR, 1);
    lim.setThreshold(-6.0f);

    // Feed loud signal
    std::vector<float> buf(1000, 1.0f);
    lim.process(buf.data(), 1000);

    lim.reset();

    // After reset, a quiet signal should pass through cleanly
    std::vector<float> buf2(2000, 0.1f);
    lim.process(buf2.data(), 2000);
    ASSERT_NEAR(buf2.back(), 0.1f, 0.02f);
    PASS();
}

int main() { return runAllTests(); }
