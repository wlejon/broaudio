#include "test_harness.h"
#include "broaudio/dsp/compressor.h"
#include <cmath>
#include <vector>

using namespace broaudio;

static constexpr int SR = 44100;

TEST(compressor_transparent_below_threshold) {
    Compressor c;
    c.init(SR);
    c.threshold = 0.7f;
    c.ratio = 4.0f;

    // Feed a quiet signal (well below threshold)
    std::vector<float> buf(1000, 0.3f);
    c.process(buf.data(), static_cast<int>(buf.size()));

    // After envelope settles, output should be ~0.3 (no compression)
    ASSERT_NEAR(buf.back(), 0.3f, 0.05f);
    PASS();
}

TEST(compressor_reduces_gain_above_threshold) {
    Compressor c;
    c.init(SR);
    c.threshold = 0.5f;
    c.ratio = 4.0f;

    // Feed a loud constant signal
    std::vector<float> buf(10000, 0.9f);
    c.process(buf.data(), static_cast<int>(buf.size()));

    // After settling, output should be compressed below 0.9
    float out = buf.back();
    ASSERT_LT(out, 0.85f);
    ASSERT_GT(out, 0.3f);
    PASS();
}

TEST(compressor_stereo_links_channels) {
    Compressor c;
    c.init(SR);
    c.threshold = 0.5f;
    c.ratio = 4.0f;

    // Stereo buffer: L loud, R quiet
    int frames = 5000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        buf[i * 2] = 0.9f;      // L
        buf[i * 2 + 1] = 0.1f;  // R
    }
    c.processStereo(buf.data(), frames);

    // Both channels should be compressed by the same gain (linked envelope)
    float outL = buf[(frames - 1) * 2];
    float outR = buf[(frames - 1) * 2 + 1];

    // L should be compressed
    ASSERT_LT(outL, 0.85f);
    // R should also be scaled down (same gain applied)
    ASSERT_LT(outR, 0.1f);
    PASS();
}

TEST(compressor_ratio_1_no_compression) {
    Compressor c;
    c.init(SR);
    c.threshold = 0.5f;
    c.ratio = 1.0f;  // no compression

    std::vector<float> buf(10000, 0.9f);
    c.process(buf.data(), static_cast<int>(buf.size()));

    // With ratio=1, gain should be ~1.0 even above threshold
    ASSERT_NEAR(buf.back(), 0.9f, 0.05f);
    PASS();
}

// --- Exact ratio verification ---

// Compressor formula: when envelope > threshold,
//   target = threshold + (envelope - threshold) / ratio
//   gain = target / envelope
// With a long steady-state signal, envelope ≈ input level.

TEST(compressor_exact_ratio_4_to_1) {
    // Input = 0.8, threshold = 0.4, ratio = 4:1
    // Expected output = 0.4 + (0.8 - 0.4) / 4 = 0.4 + 0.1 = 0.5
    Compressor c;
    c.init(SR);
    c.threshold = 0.4f;
    c.ratio = 4.0f;

    // Long buffer for envelope to fully settle
    std::vector<float> buf(50000, 0.8f);
    c.process(buf.data(), static_cast<int>(buf.size()));

    float output = buf.back();
    ASSERT_NEAR(output, 0.5f, 0.02f);
    PASS();
}

TEST(compressor_exact_ratio_2_to_1) {
    // Input = 1.0, threshold = 0.5, ratio = 2:1
    // Expected: 0.5 + (1.0 - 0.5) / 2 = 0.5 + 0.25 = 0.75
    Compressor c;
    c.init(SR);
    c.threshold = 0.5f;
    c.ratio = 2.0f;

    std::vector<float> buf(50000, 1.0f);
    c.process(buf.data(), static_cast<int>(buf.size()));

    ASSERT_NEAR(buf.back(), 0.75f, 0.02f);
    PASS();
}

TEST(compressor_exact_ratio_10_to_1) {
    // Near-limiting: input = 0.9, threshold = 0.3, ratio = 10:1
    // Expected: 0.3 + (0.9 - 0.3) / 10 = 0.3 + 0.06 = 0.36
    Compressor c;
    c.init(SR);
    c.threshold = 0.3f;
    c.ratio = 10.0f;

    std::vector<float> buf(50000, 0.9f);
    c.process(buf.data(), static_cast<int>(buf.size()));

    ASSERT_NEAR(buf.back(), 0.36f, 0.02f);
    PASS();
}

TEST(compressor_gain_in_db_matches_ratio) {
    // Verify in dB domain:
    // Input = -6dB (0.5012), threshold = -12dB (0.2512), ratio = 4:1
    // Above threshold by 6dB → compressed to 6/4 = 1.5dB above threshold
    // Output = -12 + 1.5 = -10.5dB (linear ≈ 0.2985)
    Compressor c;
    c.init(SR);
    float threshDB = -12.0f;
    c.threshold = std::pow(10.0f, threshDB / 20.0f);
    c.ratio = 4.0f;

    float inputLevel = std::pow(10.0f, -6.0f / 20.0f);
    std::vector<float> buf(50000, inputLevel);
    c.process(buf.data(), static_cast<int>(buf.size()));

    float outputDB = 20.0f * std::log10(std::fabs(buf.back()));
    float expectedDB = threshDB + (-6.0f - threshDB) / 4.0f;  // -12 + 1.5 = -10.5
    ASSERT_NEAR(outputDB, expectedDB, 0.5f);  // within 0.5dB
    PASS();
}

TEST(compressor_stereo_applies_equal_gain_to_both_channels) {
    // When L is loud and R is quiet, both should get the same gain factor
    Compressor c;
    c.init(SR);
    c.threshold = 0.3f;
    c.ratio = 4.0f;

    int frames = 50000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        buf[i * 2] = 0.9f;      // L loud
        buf[i * 2 + 1] = 0.45f; // R half as loud
    }
    c.processStereo(buf.data(), frames);

    float outL = buf[(frames - 1) * 2];
    float outR = buf[(frames - 1) * 2 + 1];
    // Gain is computed from max(L,R) = 0.9. Both channels get same gain.
    // Expected gain = target/envelope = (0.3 + 0.6/4) / 0.9 = 0.45/0.9 = 0.5
    float expectedGain = 0.5f;
    ASSERT_NEAR(outL, 0.9f * expectedGain, 0.03f);
    ASSERT_NEAR(outR, 0.45f * expectedGain, 0.03f);
    PASS();
}

int main() { return runAllTests(); }
