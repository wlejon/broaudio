#include "test_harness.h"
#include "broaudio/synth/voice_allocator.h"
#include "broaudio/engine.h"

using namespace broaudio;

// Engine without init() — voice management works, just no audio device.
// currentTime() returns 0, so we pass explicit `when` values for time-dependent tests.

TEST(allocate_voices_up_to_max) {
    Engine engine;
    VoiceAllocator alloc(engine, 4);

    int v1 = alloc.noteOn(60, 1.0f, 1.0);
    int v2 = alloc.noteOn(62, 1.0f, 2.0);
    int v3 = alloc.noteOn(64, 1.0f, 3.0);
    int v4 = alloc.noteOn(67, 1.0f, 4.0);

    ASSERT_TRUE(v1 >= 0);
    ASSERT_TRUE(v2 >= 0);
    ASSERT_TRUE(v3 >= 0);
    ASSERT_TRUE(v4 >= 0);
    ASSERT_EQ(alloc.activeVoiceCount(), 4);
    PASS();
}

TEST(note_off_frees_slot) {
    Engine engine;
    VoiceAllocator alloc(engine, 4);

    alloc.noteOn(60, 1.0f, 1.0);
    alloc.noteOn(62, 1.0f, 2.0);
    ASSERT_EQ(alloc.activeVoiceCount(), 2);

    alloc.noteOff(60, 3.0);
    ASSERT_EQ(alloc.activeVoiceCount(), 1);

    alloc.noteOff(62, 4.0);
    ASSERT_EQ(alloc.activeVoiceCount(), 0);
    PASS();
}

TEST(voice_for_note_returns_correct_id) {
    Engine engine;
    VoiceAllocator alloc(engine, 4);

    int v1 = alloc.noteOn(60, 1.0f, 1.0);
    alloc.noteOn(62, 1.0f, 2.0);

    ASSERT_EQ(alloc.voiceForNote(60), v1);
    ASSERT_EQ(alloc.voiceForNote(99), -1);  // not playing
    PASS();
}

TEST(all_notes_off_releases_all) {
    Engine engine;
    VoiceAllocator alloc(engine, 4);

    alloc.noteOn(60, 1.0f, 1.0);
    alloc.noteOn(62, 1.0f, 2.0);
    alloc.noteOn(64, 1.0f, 3.0);
    ASSERT_EQ(alloc.activeVoiceCount(), 3);

    alloc.allNotesOff(4.0);
    ASSERT_EQ(alloc.activeVoiceCount(), 0);
    PASS();
}

// --- Steal policy: None ---

TEST(steal_none_drops_note_when_full) {
    Engine engine;
    VoiceAllocator alloc(engine, 2);
    alloc.setStealPolicy(StealPolicy::None);

    alloc.noteOn(60, 1.0f, 1.0);
    alloc.noteOn(62, 1.0f, 2.0);
    int v3 = alloc.noteOn(64, 1.0f, 3.0);

    ASSERT_EQ(v3, -1);  // dropped
    ASSERT_EQ(alloc.activeVoiceCount(), 2);
    PASS();
}

// --- Steal policy: Oldest ---

TEST(steal_oldest_replaces_earliest_voice) {
    Engine engine;
    VoiceAllocator alloc(engine, 2);
    alloc.setStealPolicy(StealPolicy::Oldest);

    int v1 = alloc.noteOn(60, 1.0f, 1.0);  // oldest
    alloc.noteOn(62, 1.0f, 2.0);

    // Pool full — should steal v1 (oldest, started at t=1)
    int v3 = alloc.noteOn(64, 1.0f, 3.0);
    ASSERT_TRUE(v3 >= 0);
    ASSERT_EQ(alloc.activeVoiceCount(), 2);

    // Note 60 should no longer be playing
    ASSERT_EQ(alloc.voiceForNote(60), -1);
    // Note 64 should be playing
    ASSERT_TRUE(alloc.voiceForNote(64) >= 0);
    PASS();
}

// --- Steal policy: Quietest ---

TEST(steal_quietest_replaces_lowest_velocity) {
    Engine engine;
    VoiceAllocator alloc(engine, 3);
    alloc.setStealPolicy(StealPolicy::Quietest);

    alloc.noteOn(60, 0.9f, 1.0);   // loud
    alloc.noteOn(62, 0.2f, 2.0);   // quiet — should be stolen
    alloc.noteOn(64, 0.8f, 3.0);   // loud

    // Pool full — should steal note 62 (lowest velocity)
    int v4 = alloc.noteOn(67, 1.0f, 4.0);
    ASSERT_TRUE(v4 >= 0);
    ASSERT_EQ(alloc.activeVoiceCount(), 3);

    // Note 62 should be gone
    ASSERT_EQ(alloc.voiceForNote(62), -1);
    // Notes 60, 64, 67 should still be playing
    ASSERT_TRUE(alloc.voiceForNote(60) >= 0);
    ASSERT_TRUE(alloc.voiceForNote(64) >= 0);
    ASSERT_TRUE(alloc.voiceForNote(67) >= 0);
    PASS();
}

// --- Steal policy: SameNote ---

TEST(steal_same_note_prefers_matching_note) {
    Engine engine;
    VoiceAllocator alloc(engine, 2);
    alloc.setStealPolicy(StealPolicy::SameNote);

    alloc.noteOn(60, 1.0f, 1.0);
    alloc.noteOn(62, 1.0f, 2.0);

    // Retrigger note 60 — should steal the existing note 60, not note 62
    int v3 = alloc.noteOn(60, 0.8f, 3.0);
    ASSERT_TRUE(v3 >= 0);
    ASSERT_EQ(alloc.activeVoiceCount(), 2);

    // Both 60 and 62 should be playing
    ASSERT_TRUE(alloc.voiceForNote(60) >= 0);
    ASSERT_TRUE(alloc.voiceForNote(62) >= 0);
    PASS();
}

TEST(steal_same_note_falls_back_to_oldest) {
    Engine engine;
    VoiceAllocator alloc(engine, 2);
    alloc.setStealPolicy(StealPolicy::SameNote);

    alloc.noteOn(60, 1.0f, 1.0);  // oldest
    alloc.noteOn(62, 1.0f, 2.0);

    // Note 64 has no same-note match — falls back to oldest (note 60)
    int v3 = alloc.noteOn(64, 1.0f, 3.0);
    ASSERT_TRUE(v3 >= 0);
    ASSERT_EQ(alloc.activeVoiceCount(), 2);

    ASSERT_EQ(alloc.voiceForNote(60), -1);  // stolen
    ASSERT_TRUE(alloc.voiceForNote(62) >= 0);
    ASSERT_TRUE(alloc.voiceForNote(64) >= 0);
    PASS();
}

// --- Voice setup callback ---

TEST(voice_setup_callback_called) {
    Engine engine;
    VoiceAllocator alloc(engine, 4);

    int callbackNote = -1;
    float callbackVelocity = -1.0f;
    alloc.setVoiceSetup([&](int /*voiceId*/, int note, float velocity) {
        callbackNote = note;
        callbackVelocity = velocity;
    });

    alloc.noteOn(72, 0.6f, 1.0);
    ASSERT_EQ(callbackNote, 72);
    ASSERT_NEAR(callbackVelocity, 0.6f, 0.001f);
    PASS();
}

// --- setMaxVoices ---

TEST(set_max_voices_grows_pool) {
    Engine engine;
    VoiceAllocator alloc(engine, 2);

    alloc.noteOn(60, 1.0f, 1.0);
    alloc.noteOn(62, 1.0f, 2.0);
    ASSERT_EQ(alloc.maxVoices(), 2);

    alloc.setMaxVoices(4);
    ASSERT_EQ(alloc.maxVoices(), 4);

    // Now we can add more without stealing
    alloc.setStealPolicy(StealPolicy::None);
    int v3 = alloc.noteOn(64, 1.0f, 3.0);
    ASSERT_TRUE(v3 >= 0);
    ASSERT_EQ(alloc.activeVoiceCount(), 3);
    PASS();
}

TEST(set_max_voices_shrinks_pool) {
    Engine engine;
    VoiceAllocator alloc(engine, 4);

    alloc.noteOn(60, 1.0f, 1.0);
    alloc.noteOn(62, 1.0f, 2.0);
    alloc.noteOn(64, 1.0f, 3.0);

    alloc.setMaxVoices(2);
    ASSERT_EQ(alloc.maxVoices(), 2);
    // The third voice (in slot 2) was released during shrink
    ASSERT_EQ(alloc.activeVoiceCount(), 2);
    PASS();
}

int main() { return runAllTests(); }
