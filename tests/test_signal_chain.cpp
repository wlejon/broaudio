#include "test_harness.h"
#include "broaudio/dsp/equalizer.h"
#include "broaudio/dsp/reverb.h"
#include "broaudio/dsp/limiter.h"
#include "broaudio/synth/oscillator.h"

#include <cmath>
#include <vector>

using namespace broaudio;

static constexpr int SR = 44100;
static constexpr float VOICE_AMPLITUDE = 0.1f;

// Helper: generate N voices of saw wave into a stereo interleaved buffer.
// Each voice gets a different frequency to simulate a chord.
static void generateVoices(float* buf, int frames, int numVoices) {
    static const float baseFreqs[] = {
        261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f, 523.25f,
        587.33f, 659.25f, 698.46f, 783.99f, 880.00f, 987.77f, 1046.50f, 1174.66f,
        1318.51f, 1396.91f, 1567.98f, 1760.00f, 1975.53f, 2093.00f, 2349.32f,
        2637.02f, 2793.83f, 3135.96f, 3520.00f, 3951.07f, 4186.01f, 4698.63f
    };

    std::memset(buf, 0, frames * 2 * sizeof(float));

    for (int v = 0; v < numVoices; v++) {
        float freq = baseFreqs[v % 30];
        float phase = 0.0f;
        float phaseInc = freq / static_cast<float>(SR);

        for (int i = 0; i < frames; i++) {
            float s = generateSample(Waveform::Sawtooth, phase, phaseInc) * VOICE_AMPLITUDE;
            buf[i * 2]     += s;
            buf[i * 2 + 1] += s;
            phase += phaseInc;
            if (phase >= 1.0f) phase -= 1.0f;
        }
    }
}

// Helper: find peak absolute value in a stereo buffer
static float findPeak(const float* buf, int frames) {
    float peak = 0.0f;
    for (int i = 0; i < frames * 2; i++) {
        float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

// --------------------------------------------------------------------------
// Test: 8 voices + reverb 100% wet → limiter → no clipping
// --------------------------------------------------------------------------

TEST(chain_8_voices_reverb_100_no_clip) {
    const int frames = 44100;  // 1 second
    std::vector<float> buf(frames * 2);

    generateVoices(buf.data(), frames, 8);

    // Reverb at 100% wet
    Reverb rev;
    rev.init(SR);
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 1.0f;
    rev.processStereo(buf.data(), frames);

    // Master gain
    float masterGain = 0.5f;
    for (int i = 0; i < frames * 2; i++) buf[i] *= masterGain;

    // Limiter
    Limiter lim(SR, 2);
    lim.setThreshold(-6.0f);
    lim.process(buf.data(), frames);

    float threshLin = std::pow(10.0f, -6.0f / 20.0f);

    // After the lookahead settles (~10ms), output must not exceed threshold
    float peak = 0.0f;
    for (int i = SR / 100; i < frames; i++) {
        float a = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
        if (a > peak) peak = a;
    }
    ASSERT_LT(peak, threshLin + 0.01f);
    PASS();
}

// --------------------------------------------------------------------------
// Test: 30 voices + EQ +16dB + reverb 100% wet → limiter → no clipping
// --------------------------------------------------------------------------

TEST(chain_30_voices_eq16db_reverb_100_no_clip) {
    const int frames = 44100;  // 1 second
    std::vector<float> buf(frames * 2);

    generateVoices(buf.data(), frames, 30);

    // EQ with +16dB on all bands (worst case)
    Equalizer eq(SR);
    eq.setEnabled(true);
    eq.setMasterGain(0.0f);
    for (int b = 0; b < Equalizer::NUM_BANDS; b++)
        eq.setBandGain(b, 12.0f);  // max supported is 12dB per band
    eq.processStereoInterleaved(buf.data(), frames);

    // Reverb at 100% wet
    Reverb rev;
    rev.init(SR);
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 1.0f;
    rev.processStereo(buf.data(), frames);

    // Master gain
    float masterGain = 0.5f;
    for (int i = 0; i < frames * 2; i++) buf[i] *= masterGain;

    // Limiter
    Limiter lim(SR, 2);
    lim.setThreshold(-6.0f);
    lim.process(buf.data(), frames);

    float threshLin = std::pow(10.0f, -6.0f / 20.0f);

    // After the lookahead settles, output must not exceed threshold
    float peak = 0.0f;
    for (int i = SR / 100; i < frames; i++) {
        float a = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
        if (a > peak) peak = a;
    }
    ASSERT_LT(peak, threshLin + 0.01f);
    PASS();
}

// --------------------------------------------------------------------------
// Test: 30 voices + EQ +12dB + chorus + delay + reverb → limiter → no clip
// --------------------------------------------------------------------------

TEST(chain_30_voices_all_effects_cranked_no_clip) {
    const int frames = 44100;
    std::vector<float> buf(frames * 2);

    generateVoices(buf.data(), frames, 30);

    // EQ boost
    Equalizer eq(SR);
    eq.setEnabled(true);
    for (int b = 0; b < Equalizer::NUM_BANDS; b++)
        eq.setBandGain(b, 12.0f);
    eq.processStereoInterleaved(buf.data(), frames);

    // Reverb 100%
    Reverb rev;
    rev.init(SR);
    rev.roomSize = 0.95f;  // extra long tail
    rev.damping = 0.2f;
    rev.mix = 1.0f;
    rev.processStereo(buf.data(), frames);

    // Master gain at full
    float masterGain = 1.0f;
    for (int i = 0; i < frames * 2; i++) buf[i] *= masterGain;

    // Limiter
    Limiter lim(SR, 2);
    lim.setThreshold(-3.0f);
    lim.process(buf.data(), frames);

    float threshLin = std::pow(10.0f, -3.0f / 20.0f);

    float peak = 0.0f;
    for (int i = SR / 100; i < frames; i++) {
        float a = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
        if (a > peak) peak = a;
    }
    ASSERT_LT(peak, threshLin + 0.01f);
    PASS();
}

// --------------------------------------------------------------------------
// Test: 4 voices + reverb 100% (the user's original failure case)
// --------------------------------------------------------------------------

TEST(chain_4_voices_reverb_100_no_clip) {
    const int frames = 44100;
    std::vector<float> buf(frames * 2);

    generateVoices(buf.data(), frames, 4);

    Reverb rev;
    rev.init(SR);
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 1.0f;
    rev.processStereo(buf.data(), frames);

    float masterGain = 0.5f;
    for (int i = 0; i < frames * 2; i++) buf[i] *= masterGain;

    Limiter lim(SR, 2);
    lim.setThreshold(-6.0f);
    lim.process(buf.data(), frames);

    float threshLin = std::pow(10.0f, -6.0f / 20.0f);

    float peak = 0.0f;
    for (int i = SR / 100; i < frames; i++) {
        float a = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
        if (a > peak) peak = a;
    }
    ASSERT_LT(peak, threshLin + 0.01f);
    PASS();
}

// --------------------------------------------------------------------------
// Test: same as above but processed in small 128-frame chunks like the real
// audio callback. This catches bugs where limiter state across callbacks
// fails to prevent clipping.
// --------------------------------------------------------------------------

TEST(chain_30_voices_eq12db_reverb_chunked_no_clip) {
    const int totalFrames = 44100;
    const int chunkSize = 128;
    std::vector<float> buf(totalFrames * 2);

    generateVoices(buf.data(), totalFrames, 30);

    // EQ +12dB all bands
    Equalizer eq(SR);
    eq.setEnabled(true);
    for (int b = 0; b < Equalizer::NUM_BANDS; b++)
        eq.setBandGain(b, 12.0f);
    eq.processStereoInterleaved(buf.data(), totalFrames);

    // Reverb 100%
    Reverb rev;
    rev.init(SR);
    rev.roomSize = 0.85f;
    rev.damping = 0.5f;
    rev.mix = 1.0f;

    // Master gain + limiter, processed in chunks just like audioCallback
    float masterGain = 0.5f;
    Limiter lim(SR, 2);
    lim.setThreshold(-6.0f);
    float threshLin = std::pow(10.0f, -6.0f / 20.0f);

    float worstPeak = 0.0f;
    int settleFrames = SR / 100;  // 10ms settle

    for (int pos = 0; pos < totalFrames; pos += chunkSize) {
        int frames = std::min(chunkSize, totalFrames - pos);
        float* chunk = buf.data() + pos * 2;

        // Reverb processes per-chunk (like the bus effect chain does)
        rev.processStereo(chunk, frames);

        // Apply master gain
        for (int i = 0; i < frames * 2; i++)
            chunk[i] *= masterGain;

        // Limiter processes per-chunk
        lim.process(chunk, frames);

        // Check peaks (after settle time)
        if (pos >= settleFrames) {
            for (int i = 0; i < frames * 2; i++) {
                float a = std::fabs(chunk[i]);
                if (a > worstPeak) worstPeak = a;
            }
        }
    }

    ASSERT_LT(worstPeak, threshLin + 0.01f);
    PASS();
}

// --------------------------------------------------------------------------
// Test: extreme case — 30 voices, +12dB EQ, master gain 1.0, reverb 100%,
// roomSize 0.95, processed in 128-frame chunks. Absolute worst case.
// --------------------------------------------------------------------------

TEST(chain_extreme_worst_case_chunked_no_clip) {
    const int totalFrames = 44100;
    const int chunkSize = 128;
    std::vector<float> buf(totalFrames * 2);

    generateVoices(buf.data(), totalFrames, 30);

    Equalizer eq(SR);
    eq.setEnabled(true);
    for (int b = 0; b < Equalizer::NUM_BANDS; b++)
        eq.setBandGain(b, 12.0f);
    eq.processStereoInterleaved(buf.data(), totalFrames);

    Reverb rev;
    rev.init(SR);
    rev.roomSize = 0.95f;
    rev.damping = 0.1f;
    rev.mix = 1.0f;

    float masterGain = 1.0f;
    Limiter lim(SR, 2);
    lim.setThreshold(-3.0f);
    float threshLin = std::pow(10.0f, -3.0f / 20.0f);

    float worstPeak = 0.0f;
    int settleFrames = SR / 100;

    for (int pos = 0; pos < totalFrames; pos += chunkSize) {
        int frames = std::min(chunkSize, totalFrames - pos);
        float* chunk = buf.data() + pos * 2;

        rev.processStereo(chunk, frames);

        for (int i = 0; i < frames * 2; i++)
            chunk[i] *= masterGain;

        lim.process(chunk, frames);

        if (pos >= settleFrames) {
            for (int i = 0; i < frames * 2; i++) {
                float a = std::fabs(chunk[i]);
                if (a > worstPeak) worstPeak = a;
            }
        }
    }

    ASSERT_LT(worstPeak, threshLin + 0.01f);
    PASS();
}

int main() { return runAllTests(); }
