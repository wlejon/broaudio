#include "test_harness.h"
#include "broaudio/engine.h"

#include <vector>

using namespace broaudio;

// These tests exercise the scheduled event ring buffer at the struct level
// (no SDL audio device needed).

TEST(scheduled_event_ring_write_read) {
    // Test the ring buffer logic directly via the public schedule methods.
    // We can't easily test sample-accurate dispatch without an audio device,
    // but we can verify the API compiles and the ring doesn't crash.
    Engine engine;
    // Note: engine is not init'd (no SDL), but schedule methods just write
    // to the ring buffer which doesn't require init.
    engine.scheduleNoteOn(1, 0.5);
    engine.scheduleNoteOff(1, 1.0);
    engine.scheduleNoteOn(2, 1.5);
    // No crash = success. The events will be drained in the audio callback.
    PASS();
}

TEST(scheduled_event_ring_overflow_graceful) {
    Engine engine;
    // Fill the ring buffer. Should not crash or hang.
    for (int i = 0; i < 5000; i++) {
        engine.scheduleNoteOn(i % 16, static_cast<double>(i) * 0.001);
    }
    // Some events will be dropped (ring full), but no crash.
    PASS();
}

TEST(schedule_api_types_exist) {
    // Compile-time test: make sure the API signatures exist.
    Engine engine;
    engine.scheduleNoteOn(0, 0.0);
    engine.scheduleNoteOff(0, 0.0);
    PASS();
}

int main() { return runAllTests(); }
