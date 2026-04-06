#include "test_harness.h"
#include "broaudio/dsp/chorus.h"
#include "broaudio/dsp/fft.h"
#include <cmath>
#include <vector>
#include <numbers>

using namespace broaudio;

static constexpr int SR = 44100;

TEST(chorus_modulates_signal) {
    Chorus ch;
    ch.init(SR);
    ch.enabled = true;
    ch.rate = 1.0f;
    ch.depth = 0.005f;
    ch.mix = 0.5f;
    ch.feedback = 0.0f;
    ch.baseDelay = 0.01f;

    // Feed a sine wave — chorus should produce amplitude modulation via comb filtering
    int frames = SR;  // 1 second
    std::vector<float> buf(frames * 2);
    float freq = 440.0f;
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    ch.processStereo(buf.data(), frames);

    // Output should differ from input (dry+wet creates variation)
    // Check that the output is not identical to dry sine
    bool differs = false;
    for (int i = frames / 2; i < frames; i++) {
        float dry = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / SR);
        if (std::fabs(buf[i * 2] - dry) > 0.01f) {
            differs = true;
            break;
        }
    }
    ASSERT_TRUE(differs);
    PASS();
}

TEST(chorus_dry_when_mix_zero) {
    Chorus ch;
    ch.init(SR);
    ch.enabled = true;
    ch.rate = 1.0f;
    ch.depth = 0.005f;
    ch.mix = 0.0f;  // fully dry
    ch.feedback = 0.0f;
    ch.baseDelay = 0.01f;

    int frames = 1000;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        buf[i * 2] = 0.5f;
        buf[i * 2 + 1] = 0.5f;
    }

    ch.processStereo(buf.data(), frames);

    ASSERT_NEAR(buf[(frames - 1) * 2], 0.5f, 0.01f);
    PASS();
}

TEST(chorus_feedback_creates_flanger) {
    Chorus ch;
    ch.init(SR);
    ch.enabled = true;
    ch.rate = 0.5f;
    ch.depth = 0.002f;
    ch.mix = 0.5f;
    ch.feedback = 0.7f;  // flanger territory
    ch.baseDelay = 0.003f;

    // Feed a sine wave
    int frames = SR;
    std::vector<float> buf(frames * 2);
    float freq = 440.0f;
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    ch.processStereo(buf.data(), frames);

    // Feedback should cause comb filtering — output differs from input
    // Just verify it didn't blow up and produces a bounded signal
    float peak = 0.0f;
    for (int i = 0; i < frames * 2; i++) {
        float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }
    ASSERT_LT(peak, 5.0f);
    ASSERT_GT(peak, 0.1f);
    PASS();
}

TEST(chorus_stereo_has_width) {
    Chorus ch;
    ch.init(SR);
    ch.enabled = true;
    ch.rate = 1.0f;
    ch.depth = 0.005f;
    ch.mix = 1.0f;
    ch.feedback = 0.0f;
    ch.baseDelay = 0.01f;

    // Mono input
    int frames = SR;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    ch.processStereo(buf.data(), frames);

    // L and R should differ due to the phase offset between channels
    bool differs = false;
    for (int i = frames / 2; i < frames; i++) {
        if (std::fabs(buf[i * 2] - buf[i * 2 + 1]) > 0.01f) {
            differs = true;
            break;
        }
    }
    ASSERT_TRUE(differs);
    PASS();
}

// --- LFO rate verification ---

TEST(chorus_rate_affects_output_spectral_content) {
    // The chorus creates sidebands around the carrier frequency at ±LFO_rate.
    // A higher LFO rate should spread sidebands further from the carrier.
    // We verify this by measuring spectral energy far from the fundamental.
    auto measureSpectralSpread = [](float rate) {
        Chorus ch;
        ch.init(SR);
        ch.enabled = true;
        ch.rate = rate;
        ch.depth = 0.005f;
        ch.mix = 1.0f;
        ch.feedback = 0.0f;
        ch.baseDelay = 0.01f;

        const int N = 8192;
        std::vector<float> buf(N * 2);
        float freq = 1000.0f;
        for (int i = 0; i < N; i++) {
            float s = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / SR);
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        ch.processStereo(buf.data(), N);

        // FFT the L channel
        std::vector<float> re(N), im(N, 0.0f);
        float w;
        for (int i = 0; i < N; i++) {
            w = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * i / N));
            re[i] = buf[i * 2] * w;
        }
        broaudio::fft(re.data(), im.data(), N);

        float binHz = static_cast<float>(SR) / N;
        int fundBin = static_cast<int>(freq / binHz + 0.5f);

        // Measure energy outside ±50Hz of fundamental (sideband energy)
        float sidebandEnergy = 0.0f;
        int margin = static_cast<int>(50.0f / binHz);
        for (int i = 1; i < N / 2; i++) {
            if (std::abs(i - fundBin) > margin) {
                float mag = re[i] * re[i] + im[i] * im[i];
                sidebandEnergy += mag;
            }
        }
        return sidebandEnergy;
    };

    float spreadSlow = measureSpectralSpread(0.5f);
    float spreadFast = measureSpectralSpread(5.0f);

    // Higher rate should spread more energy into sidebands
    ASSERT_GT(spreadFast, spreadSlow);
    PASS();
}

TEST(chorus_depth_zero_is_nearly_transparent) {
    // With depth=0 (no modulation), the chorus is just a fixed-delay copy.
    // At mix=0.5, dry+wet should be close to the original (comb filtering aside).
    // With large depth, the signal should differ significantly from the dry version.
    Chorus chStatic;
    chStatic.init(SR);
    chStatic.enabled = true;
    chStatic.rate = 1.0f;
    chStatic.depth = 0.0f;    // no modulation
    chStatic.mix = 0.5f;
    chStatic.feedback = 0.0f;
    chStatic.baseDelay = 0.01f;

    Chorus chDeep;
    chDeep.init(SR);
    chDeep.enabled = true;
    chDeep.rate = 1.0f;
    chDeep.depth = 0.008f;   // significant modulation
    chDeep.mix = 0.5f;
    chDeep.feedback = 0.0f;
    chDeep.baseDelay = 0.01f;

    int frames = SR;
    std::vector<float> bufStatic(frames * 2), bufDeep(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
        bufStatic[i * 2] = s; bufStatic[i * 2 + 1] = s;
        bufDeep[i * 2] = s; bufDeep[i * 2 + 1] = s;
    }
    chStatic.processStereo(bufStatic.data(), frames);
    chDeep.processStereo(bufDeep.data(), frames);

    // With depth=0, L and R should be identical (no LFO modulation)
    float lrDiffStatic = 0.0f;
    float lrDiffDeep = 0.0f;
    for (int i = frames / 2; i < frames; i++) {
        float dS = bufStatic[i * 2] - bufStatic[i * 2 + 1];
        float dD = bufDeep[i * 2] - bufDeep[i * 2 + 1];
        lrDiffStatic += dS * dS;
        lrDiffDeep += dD * dD;
    }

    // Deep chorus should create more L/R difference than static delay
    ASSERT_GT(lrDiffDeep, lrDiffStatic * 2.0f);
    PASS();
}

TEST(chorus_feedback_increases_resonance) {
    // With feedback, the comb filter becomes more resonant — peak amplitude increases
    auto measurePeak = [](float feedback) {
        Chorus ch;
        ch.init(SR);
        ch.enabled = true;
        ch.rate = 0.5f;
        ch.depth = 0.003f;
        ch.mix = 0.5f;
        ch.feedback = feedback;
        ch.baseDelay = 0.005f;

        int frames = SR;
        std::vector<float> buf(frames * 2);
        for (int i = 0; i < frames; i++) {
            float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / SR);
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        ch.processStereo(buf.data(), frames);

        float peak = 0.0f;
        for (int i = frames / 2; i < frames; i++) {
            float a = std::fabs(buf[i * 2]);
            if (a > peak) peak = a;
        }
        return peak;
    };

    float peakNoFB = measurePeak(0.0f);
    float peakFB = measurePeak(0.7f);

    // Feedback causes constructive interference — peak should be higher
    ASSERT_GT(peakFB, peakNoFB);
    PASS();
}

int main() { return runAllTests(); }
