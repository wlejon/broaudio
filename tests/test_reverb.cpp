#include "test_harness.h"
#include "broaudio/dsp/reverb.h"
#include <cmath>
#include <vector>

using namespace broaudio;

static constexpr int SR = 44100;

TEST(reverb_impulse_produces_tail) {
    Reverb rev;
    rev.init(SR);
    rev.enabled = true;
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 1.0f;

    int frames = 4000;
    std::vector<float> buf(frames * 2, 0.0f);
    buf[0] = 1.0f;  // L impulse
    buf[1] = 1.0f;  // R impulse

    rev.processStereo(buf.data(), frames);

    // Should have a reverb tail — energy in later frames
    float energy = 0.0f;
    for (int i = 2000; i < 4000; i++) {
        energy += buf[i * 2] * buf[i * 2] + buf[i * 2 + 1] * buf[i * 2 + 1];
    }
    ASSERT_GT(energy, 0.001f);
    PASS();
}

TEST(reverb_dry_mix_zero_passes_dry) {
    Reverb rev;
    rev.init(SR);
    rev.enabled = true;
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 0.0f;  // fully dry

    int frames = 1000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        buf[i * 2] = 0.5f;
        buf[i * 2 + 1] = 0.5f;
    }

    rev.processStereo(buf.data(), frames);

    // Output should be ~dry signal
    ASSERT_NEAR(buf[(frames - 1) * 2], 0.5f, 0.05f);
    PASS();
}

TEST(reverb_clear_silences_tail) {
    Reverb rev;
    rev.init(SR);
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 1.0f;

    // Feed impulse
    std::vector<float> buf(2000, 0.0f);
    buf[0] = 1.0f; buf[1] = 1.0f;
    rev.processStereo(buf.data(), 1000);

    rev.clear();

    // Feed silence — should get silence out
    std::vector<float> buf2(2000, 0.0f);
    rev.processStereo(buf2.data(), 1000);

    float energy = 0.0f;
    for (int i = 0; i < 2000; i++) energy += buf2[i] * buf2[i];
    ASSERT_NEAR(energy, 0.0f, 1e-6f);
    PASS();
}

TEST(reverb_stereo_output_differs) {
    Reverb rev;
    rev.init(SR);
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 1.0f;

    int frames = 2000;
    std::vector<float> buf(frames * 2, 0.0f);
    buf[0] = 1.0f; buf[1] = 1.0f;
    rev.processStereo(buf.data(), frames);

    // L and R should differ (stereo spread via offset comb lengths)
    bool differs = false;
    for (int i = 500; i < frames; i++) {
        if (std::fabs(buf[i * 2] - buf[i * 2 + 1]) > 0.001f) {
            differs = true;
            break;
        }
    }
    ASSERT_TRUE(differs);
    PASS();
}

int main() { return runAllTests(); }
