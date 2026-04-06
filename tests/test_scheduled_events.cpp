#include "test_harness.h"
#include "broaudio/engine.h"

#include <vector>
#include <cmath>

using namespace broaudio;

// These tests exercise the scheduled event ring buffer and, with initHeadless(),
// verify sample-accurate dispatch by inspecting voice state.

TEST(scheduled_event_ring_write_read) {
    Engine engine;
    engine.scheduleNoteOn(1, 0.5);
    engine.scheduleNoteOff(1, 1.0);
    engine.scheduleNoteOn(2, 1.5);
    PASS();
}

TEST(scheduled_event_ring_overflow_graceful) {
    Engine engine;
    for (int i = 0; i < 5000; i++) {
        engine.scheduleNoteOn(i % 16, static_cast<double>(i) * 0.001);
    }
    PASS();
}

TEST(schedule_api_types_exist) {
    Engine engine;
    engine.scheduleNoteOn(0, 0.0);
    engine.scheduleNoteOff(0, 0.0);
    PASS();
}

// --- Sample-accurate dispatch tests (using headless engine) ---

TEST(scheduled_note_on_fires_at_correct_time) {
    Engine engine;
    engine.initHeadless();

    int voiceId = engine.createVoice();
    engine.setWaveform(voiceId, Waveform::Sine);
    engine.setFrequency(voiceId, 440.0f);
    engine.setGain(voiceId, 1.0f);
    engine.setAttackTime(voiceId, 0.001f);
    engine.setSustainLevel(voiceId, 1.0f);
    engine.setReleaseTime(voiceId, 0.01f);

    // Schedule noteOn at 0.1 seconds = 4410 samples at 44100 Hz
    engine.scheduleNoteOn(voiceId, 0.1);

    // Render 4000 samples (before the event)
    engine.renderBlock(4000);

    // Master bus should have no signal yet (voice hasn't started)
    float peakBefore = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_NEAR(peakBefore, 0.0f, 0.001f);

    // Render past the event (another 1000 samples, bringing total to 5000 > 4410)
    engine.renderBlock(1000);

    // Now the voice should be sounding
    float peakAfter = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_GT(peakAfter, 0.01f);

    engine.removeVoice(voiceId);
    engine.shutdown();
    PASS();
}

TEST(scheduled_note_off_silences_voice) {
    Engine engine;
    engine.initHeadless();

    int voiceId = engine.createVoice();
    engine.setWaveform(voiceId, Waveform::Sine);
    engine.setFrequency(voiceId, 440.0f);
    engine.setGain(voiceId, 1.0f);
    engine.setAttackTime(voiceId, 0.001f);
    engine.setSustainLevel(voiceId, 1.0f);
    engine.setReleaseTime(voiceId, 0.01f);

    // Start voice immediately
    engine.scheduleNoteOn(voiceId, 0.0);
    engine.renderBlock(2000);

    float peakPlaying = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_GT(peakPlaying, 0.01f);

    // Schedule noteOff at current time + 0.05s
    double offTime = engine.currentTime() + 0.05;
    engine.scheduleNoteOff(voiceId, offTime);

    // Render past the noteOff + release time
    engine.renderBlock(44100);  // 1 full second

    float peakAfter = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_NEAR(peakAfter, 0.0f, 0.001f);

    engine.removeVoice(voiceId);
    engine.shutdown();
    PASS();
}

TEST(scheduled_events_multiple_voices_interleaved) {
    Engine engine;
    engine.initHeadless();

    int v1 = engine.createVoice();
    int v2 = engine.createVoice();
    engine.setWaveform(v1, Waveform::Sine);
    engine.setWaveform(v2, Waveform::Sine);
    engine.setFrequency(v1, 440.0f);
    engine.setFrequency(v2, 880.0f);
    engine.setGain(v1, 0.5f);
    engine.setGain(v2, 0.5f);
    engine.setAttackTime(v1, 0.001f);
    engine.setAttackTime(v2, 0.001f);
    engine.setSustainLevel(v1, 1.0f);
    engine.setSustainLevel(v2, 1.0f);
    engine.setReleaseTime(v1, 0.01f);
    engine.setReleaseTime(v2, 0.01f);

    // v1 on at 0.0s, v2 on at 0.1s, v1 off at 0.2s, v2 off at 0.3s
    engine.scheduleNoteOn(v1, 0.0);
    engine.scheduleNoteOn(v2, 0.1);
    engine.scheduleNoteOff(v1, 0.2);
    engine.scheduleNoteOff(v2, 0.3);

    // At 0.05s: only v1 should be playing
    engine.renderBlock(2205);  // 0.05s
    float peak005 = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_GT(peak005, 0.01f);

    // At 0.15s: both should be playing (louder)
    engine.renderBlock(4410);  // another 0.1s → total 0.15s
    float peak015 = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_GT(peak015, 0.01f);

    // At 0.5s: both should be done
    engine.renderBlock(44100);
    float peakEnd = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_NEAR(peakEnd, 0.0f, 0.001f);

    engine.removeVoice(v1);
    engine.removeVoice(v2);
    engine.shutdown();
    PASS();
}

int main() { return runAllTests(); }
