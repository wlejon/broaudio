#include "test_harness.h"
#include "broaudio/sequencer/sequence.h"
#include "broaudio/synth/voice_allocator.h"
#include "broaudio/engine.h"

using namespace broaudio;

// Helper: create a minimal engine + allocator pair for testing.
// The engine won't actually produce audio (no SDL init), but the
// VoiceAllocator and Sequence only use main-thread APIs.
struct TestFixture {
    Engine engine;
    VoiceAllocator allocator;
    Sequence sequence;

    TestFixture()
        : allocator(engine, 8)
        , sequence(allocator)
    {}
};

// --- BPM ---

TEST(default_bpm) {
    TestFixture f;
    ASSERT_NEAR(f.sequence.bpm(), 120.0, 1e-9);
    PASS();
}

TEST(set_bpm) {
    TestFixture f;
    f.sequence.setBPM(140.0);
    ASSERT_NEAR(f.sequence.bpm(), 140.0, 1e-9);
    PASS();
}

TEST(bpm_clamped_low) {
    TestFixture f;
    f.sequence.setBPM(-10.0);
    ASSERT_GT(f.sequence.bpm(), 0.0);
    PASS();
}

// --- Time signature ---

TEST(default_time_signature) {
    TestFixture f;
    ASSERT_EQ(f.sequence.timeSignatureNumerator(), 4);
    ASSERT_EQ(f.sequence.timeSignatureDenominator(), 4);
    PASS();
}

TEST(set_time_signature) {
    TestFixture f;
    f.sequence.setTimeSignature(3, 4);
    ASSERT_EQ(f.sequence.timeSignatureNumerator(), 3);
    ASSERT_EQ(f.sequence.timeSignatureDenominator(), 4);
    PASS();
}

// --- Note management ---

TEST(add_note) {
    TestFixture f;
    f.sequence.addNote({0.0, 60, 0.8f, 1.0});
    ASSERT_EQ(f.sequence.noteCount(), 1);
    ASSERT_EQ(f.sequence.note(0).note, 60);
    PASS();
}

TEST(notes_sorted_by_beat) {
    TestFixture f;
    f.sequence.addNote({2.0, 62, 0.5f, 1.0});
    f.sequence.addNote({0.0, 60, 0.8f, 1.0});
    f.sequence.addNote({1.0, 61, 0.6f, 1.0});
    ASSERT_EQ(f.sequence.note(0).note, 60);
    ASSERT_EQ(f.sequence.note(1).note, 61);
    ASSERT_EQ(f.sequence.note(2).note, 62);
    PASS();
}

TEST(remove_note) {
    TestFixture f;
    f.sequence.addNote({0.0, 60, 0.8f, 1.0});
    f.sequence.addNote({1.0, 61, 0.6f, 1.0});
    f.sequence.removeNote(0);
    ASSERT_EQ(f.sequence.noteCount(), 1);
    ASSERT_EQ(f.sequence.note(0).note, 61);
    PASS();
}

TEST(clear_notes) {
    TestFixture f;
    f.sequence.addNote({0.0, 60, 0.8f, 1.0});
    f.sequence.addNote({1.0, 61, 0.6f, 1.0});
    f.sequence.clearNotes();
    ASSERT_EQ(f.sequence.noteCount(), 0);
    PASS();
}

// --- Beat/time conversion ---

TEST(beat_to_engine_time_120bpm) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    f.sequence.play(0.0);
    // At 120 BPM, beat 1 = 0.5 seconds
    double t = f.sequence.beatToEngineTime(1.0);
    ASSERT_NEAR(t, 0.5, 1e-9);
    PASS();
}

TEST(beat_to_engine_time_60bpm) {
    TestFixture f;
    f.sequence.setBPM(60.0);
    f.sequence.play(0.0);
    // At 60 BPM, beat 1 = 1.0 seconds
    double t = f.sequence.beatToEngineTime(1.0);
    ASSERT_NEAR(t, 1.0, 1e-9);
    PASS();
}

TEST(engine_time_to_beat) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    f.sequence.play(0.0);
    double beat = f.sequence.engineTimeToBeat(1.0);
    ASSERT_NEAR(beat, 2.0, 1e-9);
    PASS();
}

TEST(beat_to_time_with_offset_start) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    f.sequence.play(10.0);  // start at engine time 10
    double t = f.sequence.beatToEngineTime(2.0);
    ASSERT_NEAR(t, 11.0, 1e-9);  // 10 + 2 * 60/120
    PASS();
}

// --- Transport ---

TEST(not_playing_by_default) {
    TestFixture f;
    ASSERT_FALSE(f.sequence.isPlaying());
    ASSERT_FALSE(f.sequence.isPaused());
    PASS();
}

TEST(play_sets_playing) {
    TestFixture f;
    f.sequence.play(0.0);
    ASSERT_TRUE(f.sequence.isPlaying());
    ASSERT_FALSE(f.sequence.isPaused());
    PASS();
}

TEST(stop_clears_playing) {
    TestFixture f;
    f.sequence.play(0.0);
    f.sequence.stop();
    ASSERT_FALSE(f.sequence.isPlaying());
    PASS();
}

TEST(pause_and_resume) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    f.sequence.play(0.0);
    f.sequence.pause(0.5);  // pause at 0.5s = beat 1.0
    ASSERT_TRUE(f.sequence.isPaused());
    ASSERT_NEAR(f.sequence.currentBeat(999.0), 1.0, 1e-9);  // beat stays at 1.0

    f.sequence.resume(2.0);  // resume at engine time 2.0
    ASSERT_FALSE(f.sequence.isPaused());
    // At engine time 2.5 (0.5s after resume), should be at beat 1.0 + 1.0 = 2.0
    ASSERT_NEAR(f.sequence.currentBeat(2.5), 2.0, 1e-9);
    PASS();
}

TEST(current_beat_when_not_playing) {
    TestFixture f;
    ASSERT_NEAR(f.sequence.currentBeat(100.0), 0.0, 1e-9);
    PASS();
}

// --- Update fires notes ---

TEST(update_fires_note_on) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    // Note at beat 0.5 = 0.25 seconds at 120 BPM
    f.sequence.addNote({0.5, 60, 0.8f, 1.0});
    f.sequence.play(0.0);

    // Update at 0.3 seconds (beat 0.6) — note at beat 0.5 should fire
    f.sequence.update(0.3);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 1);
    PASS();
}

TEST(update_does_not_fire_future_note) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    f.sequence.addNote({2.0, 60, 0.8f, 1.0});
    f.sequence.play(0.0);

    // Update at 0.1 seconds (beat 0.2) — note at beat 2.0 should NOT fire
    f.sequence.update(0.1);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 0);
    PASS();
}

TEST(update_fires_note_off_after_duration) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    // Note at beat 0 with duration 0.5 beats (ends at beat 0.5 = 0.25s)
    f.sequence.addNote({0.0, 60, 0.8f, 0.5});
    f.sequence.play(0.0);

    // First update at 0.1s (beat 0.2) — note on fires
    f.sequence.update(0.1);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 1);

    // Second update at 0.3s (beat 0.6) — note off should have fired
    f.sequence.update(0.3);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 0);
    PASS();
}

TEST(multiple_notes_fire_in_order) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    f.sequence.addNote({0.0, 60, 0.8f, 2.0});
    f.sequence.addNote({0.5, 64, 0.7f, 2.0});
    f.sequence.addNote({1.0, 67, 0.6f, 2.0});
    f.sequence.play(0.0);

    // Update at 0.1s (beat 0.2) — only first note
    f.sequence.update(0.1);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 1);

    // Update at 0.3s (beat 0.6) — first two notes
    f.sequence.update(0.3);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 2);

    // Update at 0.6s (beat 1.2) — all three
    f.sequence.update(0.6);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 3);
    PASS();
}

// --- Looping ---

TEST(loop_wraps_around) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    f.sequence.addNote({0.0, 60, 0.8f, 0.25});
    f.sequence.setLoopEnabled(true);
    f.sequence.setLoopRange(0.0, 1.0);  // loop every 1 beat (0.5s)
    f.sequence.play(0.0);

    // First pass: update at 0.1s (beat 0.2) — note fires
    f.sequence.update(0.1);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 1);

    // Note off at beat 0.25
    f.sequence.update(0.2);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 0);

    // Hit loop boundary at 0.5s (beat 1.0) — wraps back to beat 0
    f.sequence.update(0.5);

    // Second pass: update at 0.6s — note should fire again
    f.sequence.update(0.6);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 1);
    PASS();
}

TEST(stop_releases_active_notes) {
    TestFixture f;
    f.sequence.setBPM(120.0);
    f.sequence.addNote({0.0, 60, 0.8f, 10.0});  // long note
    f.sequence.play(0.0);
    f.sequence.update(0.1);
    ASSERT_EQ(f.allocator.activeVoiceCount(), 1);

    f.sequence.stop();
    ASSERT_EQ(f.allocator.activeVoiceCount(), 0);
    PASS();
}

// --- Loop range ---

TEST(loop_range_stored) {
    TestFixture f;
    f.sequence.setLoopRange(2.0, 6.0);
    ASSERT_NEAR(f.sequence.loopStartBeat(), 2.0, 1e-9);
    ASSERT_NEAR(f.sequence.loopEndBeat(), 6.0, 1e-9);
    PASS();
}

int main() { return runAllTests(); }
