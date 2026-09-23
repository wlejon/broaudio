// MIDI input through MidiInput::injectMessage: the same parse -> ring ->
// processEvents path a hardware port feeds, driven headless with no port.

#include "test_harness.h"
#include "broaudio/engine.h"
#include "broaudio/midi/midi_input.h"
#include "broaudio/synth/voice_allocator.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace broaudio;

static bool inject(MidiInput& m, std::initializer_list<uint8_t> bytes, double t = -1.0) {
    std::vector<uint8_t> v(bytes);
    return m.injectMessage(v.data(), v.size(), t);
}

TEST(parse_channel_messages) {
    MidiEvent ev;
    const uint8_t on[] = {0x93, 60, 100};
    ASSERT_TRUE(MidiInput::parseMessage(on, 3, ev));
    ASSERT_TRUE(ev.type == MidiEvent::Type::NoteOn);
    ASSERT_EQ(ev.channel, 3);
    ASSERT_EQ(ev.data1, 60);
    ASSERT_EQ(ev.data2, 100);

    const uint8_t onVel0[] = {0x90, 60, 0};  // running-status note off
    ASSERT_TRUE(MidiInput::parseMessage(onVel0, 3, ev));
    ASSERT_TRUE(ev.type == MidiEvent::Type::NoteOff);

    const uint8_t bendUp[] = {0xE0, 0x7F, 0x7F};
    ASSERT_TRUE(MidiInput::parseMessage(bendUp, 3, ev));
    ASSERT_TRUE(ev.type == MidiEvent::Type::PitchBend);
    ASSERT_EQ(ev.pitchBend, 8191);
    const uint8_t bendCenter[] = {0xE0, 0x00, 0x40};
    ASSERT_TRUE(MidiInput::parseMessage(bendCenter, 3, ev));
    ASSERT_EQ(ev.pitchBend, 0);

    const uint8_t pc[] = {0xC5, 7};
    ASSERT_TRUE(MidiInput::parseMessage(pc, 2, ev));
    ASSERT_TRUE(ev.type == MidiEvent::Type::ProgramChange);
    ASSERT_EQ(ev.channel, 5);
    ASSERT_EQ(ev.data1, 7);

    const uint8_t pressure[] = {0xD0, 90};
    ASSERT_TRUE(MidiInput::parseMessage(pressure, 2, ev));
    ASSERT_TRUE(ev.type == MidiEvent::Type::ChannelPressure);
    PASS();
}

TEST(parse_rejects_system_and_truncated_messages) {
    MidiEvent ev;
    const uint8_t sysex[] = {0xF0, 0x7E, 0xF7};
    ASSERT_FALSE(MidiInput::parseMessage(sysex, 3, ev));
    const uint8_t clock[] = {0xF8};
    ASSERT_FALSE(MidiInput::parseMessage(clock, 1, ev));
    const uint8_t shortNote[] = {0x90, 60};
    ASSERT_FALSE(MidiInput::parseMessage(shortNote, 2, ev));
    ASSERT_FALSE(MidiInput::parseMessage(nullptr, 0, ev));
    PASS();
}

TEST(inject_note_on_off_drives_allocator) {
    Engine e; e.initHeadless();
    MidiInput midi(e);
    VoiceAllocator alloc(e, 4);
    midi.connectToAllocator(&alloc);

    ASSERT_TRUE(inject(midi, {0x90, 60, 127}));
    ASSERT_TRUE(inject(midi, {0x90, 64, 64}));
    // Nothing is dispatched until processEvents.
    ASSERT_EQ(alloc.activeVoiceCount(), 0);
    midi.processEvents();
    ASSERT_EQ(alloc.activeVoiceCount(), 2);
    ASSERT_TRUE(alloc.voiceForNote(60) >= 0);

    ASSERT_TRUE(inject(midi, {0x80, 60, 0}));
    ASSERT_TRUE(inject(midi, {0x90, 64, 0}));  // velocity 0 = note off
    midi.processEvents();
    ASSERT_EQ(alloc.activeVoiceCount(), 0);
    PASS();
}

TEST(inject_note_on_makes_sound) {
    Engine e; e.initHeadless();
    MidiInput midi(e);
    VoiceAllocator alloc(e, 4);
    midi.connectToAllocator(&alloc);
    inject(midi, {0x90, 69, 127});
    midi.processEvents();
    e.renderBlock(4096);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);
    PASS();
}

TEST(inject_dispatches_callbacks_in_order) {
    Engine e; e.initHeadless();
    MidiInput midi(e);

    std::vector<MidiEvent> raw;
    int ccChannel = -1, ccValue = -1;
    int bendChannel = -1, bendValue = 0;
    midi.onRawEvent([&](const MidiEvent& ev) { raw.push_back(ev); });
    midi.onControlChange(74, [&](uint8_t ch, uint8_t cc, uint8_t v) {
        ASSERT_EQ(cc, 74);
        ccChannel = ch;
        ccValue = v;
    });
    midi.onPitchBend([&](uint8_t ch, int16_t v) { bendChannel = ch; bendValue = v; });

    ASSERT_TRUE(inject(midi, {0xB2, 74, 99}, 1.5));
    ASSERT_TRUE(inject(midi, {0xB2, 10, 5}));       // CC with no handler: raw only
    ASSERT_TRUE(inject(midi, {0xE1, 0x00, 0x00}));  // full bend down
    ASSERT_FALSE(inject(midi, {0xF8}));             // clock: ignored, never queued
    midi.processEvents();

    ASSERT_EQ(raw.size(), static_cast<size_t>(3));
    ASSERT_TRUE(raw[0].type == MidiEvent::Type::ControlChange);
    ASSERT_NEAR(raw[0].timestamp, 1.5, 1e-9);
    ASSERT_TRUE(raw[1].type == MidiEvent::Type::ControlChange);
    ASSERT_TRUE(raw[2].type == MidiEvent::Type::PitchBend);
    ASSERT_EQ(ccChannel, 2);
    ASSERT_EQ(ccValue, 99);
    ASSERT_EQ(bendChannel, 1);
    ASSERT_EQ(bendValue, -8192);

    // Drained: a second pass dispatches nothing.
    raw.clear();
    midi.processEvents();
    ASSERT_TRUE(raw.empty());
    PASS();
}

TEST(inject_default_timestamp_is_engine_time) {
    Engine e; e.initHeadless();
    e.renderBlock(4410);
    MidiInput midi(e);
    double stamp = -1.0;
    midi.onRawEvent([&](const MidiEvent& ev) { stamp = ev.timestamp; });
    inject(midi, {0x90, 60, 1});
    midi.processEvents();
    ASSERT_NEAR(stamp, e.currentTime(), 1e-9);
    PASS();
}

TEST(inject_drops_when_ring_full) {
    Engine e; e.initHeadless();
    MidiInput midi(e);
    int accepted = 0;
    for (int i = 0; i < 1100; i++) {
        if (inject(midi, {0xB0, 1, static_cast<uint8_t>(i & 0x7F)})) accepted++;
    }
    ASSERT_EQ(accepted, 1024);
    int seen = 0;
    midi.onRawEvent([&](const MidiEvent&) { seen++; });
    midi.processEvents();
    ASSERT_EQ(seen, 1024);
    // Space again after the drain.
    ASSERT_TRUE(inject(midi, {0xB0, 1, 1}));
    PASS();
}

int main() { return runAllTests(); }
