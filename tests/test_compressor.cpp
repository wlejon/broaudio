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

int main() { return runAllTests(); }
