#include "test_harness.h"
#include "broaudio/engine.h"
#include "broaudio/io/audio_file.h"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;

static std::vector<float> sineMono(int frames, float freq, int sr) {
    std::vector<float> v(frames);
    for (int i = 0; i < frames; i++)
        v[i] = std::sin(2.0f * PI * freq * i / sr);
    return v;
}

// ---------------------------------------------------------------------------
// processEffectsOffline
// ---------------------------------------------------------------------------

TEST(offline_invalid_bus_returns_empty) {
    Engine e; e.initHeadless();
    float input[100] = {0};
    auto out = e.processEffectsOffline(9999, input, 100);
    ASSERT_TRUE(out.empty());
    PASS();
}

TEST(offline_zero_samples_returns_empty) {
    Engine e; e.initHeadless();
    float dummy = 0.0f;
    auto out = e.processEffectsOffline(Engine::MASTER_BUS_ID, &dummy, 0);
    ASSERT_TRUE(out.empty());
    PASS();
}

TEST(offline_passthrough_with_no_effects) {
    Engine e; e.initHeadless();
    auto sine = sineMono(2048, 440.0f, e.sampleRate());
    auto out = e.processEffectsOffline(Engine::MASTER_BUS_ID, sine.data(), (int)sine.size());
    ASSERT_TRUE(out.size() >= (size_t)sine.size());

    // With no effects, the output should largely match the input
    float maxDiff = 0.0f;
    for (size_t i = 0; i < sine.size(); i++) {
        float d = std::fabs(out[i] - sine[i]);
        if (d > maxDiff) maxDiff = d;
    }
    ASSERT_LT(maxDiff, 0.01f);
    PASS();
}

TEST(offline_with_delay_extends_tail) {
    Engine e; e.initHeadless();
    e.setBusDelayEnabled(Engine::MASTER_BUS_ID, true);
    e.setBusDelayTime(Engine::MASTER_BUS_ID, 0.05f);
    e.setBusDelayFeedback(Engine::MASTER_BUS_ID, 0.5f);
    e.setBusDelayMix(Engine::MASTER_BUS_ID, 0.5f);

    auto sine = sineMono(2048, 440.0f, e.sampleRate());
    auto out = e.processEffectsOffline(Engine::MASTER_BUS_ID, sine.data(), (int)sine.size());
    // Output should be longer than input due to delay tail
    ASSERT_TRUE(out.size() > sine.size());
    PASS();
}

TEST(offline_with_reverb_extends_tail) {
    Engine e; e.initHeadless();
    e.setBusReverbEnabled(Engine::MASTER_BUS_ID, true);
    e.setBusReverbRoomSize(Engine::MASTER_BUS_ID, 0.8f);
    e.setBusReverbMix(Engine::MASTER_BUS_ID, 0.5f);

    auto sine = sineMono(1024, 440.0f, e.sampleRate());
    auto out = e.processEffectsOffline(Engine::MASTER_BUS_ID, sine.data(), (int)sine.size());
    ASSERT_TRUE(out.size() > sine.size());

    // After the input ends, the output should still have reverb energy
    float tailEnergy = 0.0f;
    for (size_t i = sine.size(); i < out.size(); i++) tailEnergy += out[i] * out[i];
    ASSERT_GT(tailEnergy, 0.001f);
    PASS();
}

TEST(offline_with_compressor_and_distortion) {
    Engine e; e.initHeadless();
    e.setBusCompressorEnabled(Engine::MASTER_BUS_ID, true);
    e.setBusCompressorThreshold(Engine::MASTER_BUS_ID, 0.2f);
    e.setBusCompressorRatio(Engine::MASTER_BUS_ID, 8.0f);
    e.setBusDistortionEnabled(Engine::MASTER_BUS_ID, true);
    e.setBusDistortionDrive(Engine::MASTER_BUS_ID, 3.0f);

    auto sine = sineMono(2048, 440.0f, e.sampleRate());
    auto out = e.processEffectsOffline(Engine::MASTER_BUS_ID, sine.data(), (int)sine.size());
    ASSERT_TRUE(out.size() >= sine.size());
    PASS();
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

TEST(start_stop_recording_writes_buffer) {
    Engine e; e.initHeadless();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.startVoice(v, 0.0);

    e.startRecording();
    e.renderBlock(4096);
    ASSERT_TRUE(e.isRecording());
    e.stopRecording();
    ASSERT_FALSE(e.isRecording());

    auto buf = e.getRecordBuffer();
    ASSERT_TRUE(buf.size() > 100);
    // Should have some non-zero content
    float peak = 0.0f;
    for (float s : buf) if (std::fabs(s) > peak) peak = std::fabs(s);
    ASSERT_GT(peak, 0.001f);
    PASS();
}

TEST(stop_recording_without_start_yields_empty) {
    Engine e; e.initHeadless();
    e.stopRecording();
    ASSERT_TRUE(e.getRecordBuffer().empty());
    PASS();
}

static const char* REC_WAV = "test_engine_recording.wav";

TEST(export_recording_to_wav_round_trip) {
    Engine e; e.initHeadless();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.startVoice(v, 0.0);

    e.startRecording();
    e.renderBlock(4096);
    e.stopRecording();

    bool ok = e.exportRecordingToWav(REC_WAV);
    ASSERT_TRUE(ok);

    // Read back
    AudioFileData loaded = loadAudioFile(REC_WAV);
    ASSERT_TRUE(loaded.valid());
    ASSERT_EQ(loaded.channels, 1);
    ASSERT_EQ(loaded.sampleRate, e.sampleRate());

    std::remove(REC_WAV);
    PASS();
}

TEST(export_empty_recording_fails) {
    Engine e; e.initHeadless();
    // Without recording, getRecordBuffer is empty
    bool ok = e.exportRecordingToWav(REC_WAV);
    ASSERT_FALSE(ok);
    PASS();
}

// ---------------------------------------------------------------------------
// Spectrum
// ---------------------------------------------------------------------------

TEST(get_spectrum_returns_bins) {
    Engine e; e.initHeadless();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 1000.0f);
    e.setGain(v, 1.0f);
    e.startVoice(v, 0.0);
    // Render enough to fill the output analysis buffer
    e.renderBlock(8192);

    constexpr int BINS = 256;
    std::vector<float> mags = e.getSpectrum(BINS);
    int n = static_cast<int>(mags.size());
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(n <= BINS);

    // The 1kHz peak should be somewhere in the spectrum
    int sr = e.sampleRate();
    float peakFreq = 0.0f;
    float peakMag = 0.0f;
    for (int i = 0; i < n; i++) {
        if (mags[i] > peakMag) {
            peakMag = mags[i];
            // bin k = k * sr / fftSize, fftSize = 2 * BINS
            peakFreq = (float)i * sr / (float)(2 * BINS);
        }
    }
    // Peak should be near 1000 Hz (allow generous slop for FFT bin resolution)
    ASSERT_NEAR(peakFreq, 1000.0f, sr / 64.0f);
    PASS();
}

TEST(get_spectrum_rejects_bad_bin_counts) {
    Engine e; e.initHeadless();
    // Out-of-range bin counts yield no bins at all (empty vector).
    ASSERT_TRUE(e.getSpectrum(0).empty());
    ASSERT_TRUE(e.getSpectrum(-1).empty());
    ASSERT_TRUE(e.getSpectrum(100000).empty());
    PASS();
}

// ---------------------------------------------------------------------------
// Global (master bus) shortcut filter and delay
// ---------------------------------------------------------------------------

TEST(global_filter_slot_allocate_release) {
    Engine e; e.initHeadless();
    int slot = e.allocateFilterSlot();
    ASSERT_TRUE(slot >= 0);

    e.setFilterEnabled(slot, true);
    e.setFilterType(slot, BiquadFilter::Type::Lowpass);
    e.setFilterFrequency(slot, 2000.0f);
    e.setFilterQ(slot, 0.707f);
    e.setFilterGain(slot, 0.0f);

    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sawtooth);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 0.5f);
    e.startVoice(v, 0.0);
    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);

    e.releaseFilterSlot(slot);
    PASS();
}

TEST(global_filter_invalid_slot_safe) {
    Engine e; e.initHeadless();
    e.setFilterEnabled(99, true);
    e.setFilterType(99, BiquadFilter::Type::Lowpass);
    e.setFilterFrequency(99, 1000.0f);
    e.setFilterQ(99, 1.0f);
    e.setFilterGain(99, 0.0f);
    e.releaseFilterSlot(99);
    e.releaseFilterSlot(-1);
    PASS();
}

TEST(global_filter_slot_exhaustion) {
    Engine e; e.initHeadless();
    std::vector<int> slots;
    while (true) {
        int s = e.allocateFilterSlot();
        if (s < 0) break;
        slots.push_back(s);
        if (slots.size() > 32) break;  // safety cap
    }
    ASSERT_TRUE(slots.size() > 0);
    ASSERT_TRUE(slots.size() <= (size_t)Engine::MAX_FILTERS);
    for (int s : slots) e.releaseFilterSlot(s);
    PASS();
}

TEST(global_delay_setters) {
    Engine e; e.initHeadless();
    e.setDelayEnabled(true);
    e.setDelayTime(0.2f);
    e.setDelayFeedback(0.4f);
    e.setDelayMix(0.5f);

    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 0.5f);
    e.startVoice(v, 0.0);
    e.renderBlock(4096);

    e.setDelayEnabled(false);
    PASS();
}

// ---------------------------------------------------------------------------
// Limiter setters
// ---------------------------------------------------------------------------

TEST(limiter_setters_take_effect) {
    Engine e; e.initHeadless();
    e.setLimiterEnabled(true);
    e.setLimiterThreshold(-6.0f);
    e.setLimiterRelease(100.0f);

    // Slam the limiter with loud input
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Square);
    e.setFrequency(v, 220.0f);
    e.setGain(v, 1.0f);
    e.setMasterGain(2.0f);
    e.startVoice(v, 0.0);
    e.renderBlock(8192);

    // Threshold-limited
    float L = e.getBusPeakL(Engine::MASTER_BUS_ID);
    // Should not exceed threshold + small margin
    ASSERT_LT(L, 1.0f);

    e.setLimiterEnabled(false);
    PASS();
}

int main() { return runAllTests(); }
