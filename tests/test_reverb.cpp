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

// --- Decay rate characterization ---

// Helper: measure energy in a window of stereo frames
static float measureEnergy(const float* buf, int startFrame, int endFrame) {
    float energy = 0.0f;
    for (int i = startFrame; i < endFrame; i++) {
        energy += buf[i * 2] * buf[i * 2] + buf[i * 2 + 1] * buf[i * 2 + 1];
    }
    return energy / (endFrame - startFrame);
}

TEST(reverb_tail_energy_decreases_over_time) {
    // After an impulse, overall energy should decrease over coarser time intervals.
    // Freeverb uses parallel comb filters which create local energy fluctuations,
    // so we measure in coarser windows (100ms) to smooth out ripple.
    Reverb rev;
    rev.init(SR);
    rev.enabled = true;
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 1.0f;

    int frames = SR * 2;  // 2 seconds
    std::vector<float> buf(frames * 2, 0.0f);
    buf[0] = 1.0f; buf[1] = 1.0f;
    rev.processStereo(buf.data(), frames);

    // Measure energy in 100ms windows
    int windowSize = SR / 10;  // 4410 samples
    float energy100ms = measureEnergy(buf.data(), windowSize, windowSize * 2);    // 100-200ms
    float energy500ms = measureEnergy(buf.data(), windowSize * 5, windowSize * 6); // 500-600ms
    float energy1s = measureEnergy(buf.data(), windowSize * 10, windowSize * 11);  // 1.0-1.1s
    float energy1_5s = measureEnergy(buf.data(), windowSize * 15, windowSize * 16); // 1.5-1.6s

    // Each successive window should have less energy
    ASSERT_GT(energy100ms, energy500ms);
    ASSERT_GT(energy500ms, energy1s);
    ASSERT_GT(energy1s, energy1_5s);

    // Energy at 1.5s should be significantly less than at 100ms (>10x decay)
    ASSERT_GT(energy100ms, energy1_5s * 10.0f);
    PASS();
}

TEST(reverb_larger_room_decays_slower) {
    // A larger room size should produce a longer decay tail
    int frames = SR * 2;

    // Small room
    Reverb revSmall;
    revSmall.init(SR);
    revSmall.enabled = true;
    revSmall.roomSize = 0.3f;
    revSmall.damping = 0.5f;
    revSmall.mix = 1.0f;
    std::vector<float> bufSmall(frames * 2, 0.0f);
    bufSmall[0] = 1.0f; bufSmall[1] = 1.0f;
    revSmall.processStereo(bufSmall.data(), frames);

    // Large room
    Reverb revLarge;
    revLarge.init(SR);
    revLarge.enabled = true;
    revLarge.roomSize = 0.95f;
    revLarge.damping = 0.5f;
    revLarge.mix = 1.0f;
    std::vector<float> bufLarge(frames * 2, 0.0f);
    bufLarge[0] = 1.0f; bufLarge[1] = 1.0f;
    revLarge.processStereo(bufLarge.data(), frames);

    // Compare energy at 1 second mark
    int measureStart = SR;  // 1s in
    int measureEnd = SR + SR / 10;  // 100ms window
    float energySmall = measureEnergy(bufSmall.data(), measureStart, measureEnd);
    float energyLarge = measureEnergy(bufLarge.data(), measureStart, measureEnd);

    // Large room should have significantly more energy at 1s
    ASSERT_GT(energyLarge, energySmall * 2.0f);
    PASS();
}

TEST(reverb_damping_reduces_high_frequency_content) {
    // Higher damping should produce a darker (less bright) tail
    int frames = SR;

    // Low damping (bright)
    Reverb revBright;
    revBright.init(SR);
    revBright.enabled = true;
    revBright.roomSize = 0.85f;
    revBright.damping = 0.1f;
    revBright.mix = 1.0f;
    std::vector<float> bufBright(frames * 2, 0.0f);
    bufBright[0] = 1.0f; bufBright[1] = 1.0f;
    revBright.processStereo(bufBright.data(), frames);

    // High damping (dark)
    Reverb revDark;
    revDark.init(SR);
    revDark.enabled = true;
    revDark.roomSize = 0.85f;
    revDark.damping = 0.9f;
    revDark.mix = 1.0f;
    std::vector<float> bufDark(frames * 2, 0.0f);
    bufDark[0] = 1.0f; bufDark[1] = 1.0f;
    revDark.processStereo(bufDark.data(), frames);

    // Measure "brightness" as zero-crossing rate in the tail (high freq = more crossings)
    auto countZeroCrossings = [](const float* buf, int start, int end) {
        int crossings = 0;
        for (int i = start + 1; i < end; i++) {
            if ((buf[i * 2] > 0) != (buf[(i - 1) * 2] > 0)) crossings++;
        }
        return crossings;
    };

    int tailStart = SR / 2;  // 0.5s into tail
    int tailEnd = SR;
    int crossBright = countZeroCrossings(bufBright.data(), tailStart, tailEnd);
    int crossDark = countZeroCrossings(bufDark.data(), tailStart, tailEnd);

    // Bright tail should have more zero crossings than dark tail
    ASSERT_GT(crossBright, crossDark);
    PASS();
}

int main() { return runAllTests(); }
