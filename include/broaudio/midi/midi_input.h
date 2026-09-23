#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace broaudio {

class Engine;
class VoiceAllocator;

// MIDI message parsed into a typed event
struct MidiEvent {
    enum class Type : uint8_t {
        NoteOn, NoteOff, ControlChange, PitchBend, ProgramChange, Aftertouch, ChannelPressure
    };

    Type type;
    uint8_t channel;    // 0-15
    uint8_t data1;      // note number or CC number
    uint8_t data2;      // velocity or CC value
    int16_t pitchBend;  // -8192 to +8191 (for PitchBend type)
    double timestamp;   // engine time in seconds
};

struct MidiPort {
    int index;
    std::string name;
};

// MIDI input manager. Receives MIDI messages from a hardware/virtual port
// via libremidi, pushes them into a lock-free ring buffer, and provides
// a poll interface for the audio thread or main thread to drain events.
//
// Typical usage:
//   MidiInput midi(engine);
//   midi.open(0);  // open first available port
//   // In your update loop or via connectToAllocator:
//   midi.connectToAllocator(allocator);
//   midi.processEvents();  // call regularly from main thread
class MidiInput {
public:
    explicit MidiInput(Engine& engine);
    ~MidiInput();

    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // Port enumeration
    std::vector<MidiPort> availablePorts() const;

    // Open/close a MIDI input port by index
    bool open(int portIndex);
    void close();
    bool isOpen() const { return open_; }

    // Connect to a VoiceAllocator for automatic note on/off routing
    void connectToAllocator(VoiceAllocator* allocator) { allocator_ = allocator; }

    // CC mapping: register a callback for a specific CC number
    using CcCallback = std::function<void(uint8_t channel, uint8_t cc, uint8_t value)>;
    void onControlChange(uint8_t cc, CcCallback fn);

    // Pitch bend callback
    using PitchBendCallback = std::function<void(uint8_t channel, int16_t value)>;
    void onPitchBend(PitchBendCallback fn) { pitchBendCallback_ = std::move(fn); }

    // Drain pending events and dispatch them. Call this from the main thread.
    void processEvents();

    // Raw event callback — receives all events before dispatch
    using RawCallback = std::function<void(const MidiEvent&)>;
    void onRawEvent(RawCallback fn) { rawCallback_ = std::move(fn); }

    // Injection seam: feed one raw MIDI message (status byte + data bytes) in
    // exactly as if it had arrived from the open port — same parsing, same
    // ring, dispatched by the next processEvents(). Works with no port open
    // and in builds without libremidi, which is what makes MIDI input
    // testable headless; also usable as a virtual keyboard. `timestamp` is
    // engine seconds; negative stamps it with engine.currentTime().
    //
    // Returns false when the bytes are not a channel message this class
    // understands (sysex, clock and other system messages are ignored, as
    // from a port), are truncated, or the ring is full (the event is dropped,
    // as from a port). Single-producer like the port callback: do not call
    // it while a port is open and delivering on its own thread.
    bool injectMessage(const uint8_t* bytes, size_t size, double timestamp = -1.0);

    // Parse one raw message into `event` (timestamp left 0). False for the
    // same inputs injectMessage rejects before the ring.
    static bool parseMessage(const uint8_t* bytes, size_t size, MidiEvent& event);

private:
    static constexpr int RING_SIZE = 1024;

    bool pushEvent(const MidiEvent& event);
    bool receiveMessage(const uint8_t* bytes, size_t size, double timestamp);

    Engine& engine_;
    VoiceAllocator* allocator_ = nullptr;
    bool open_ = false;

    // Lock-free SPSC ring buffer for MIDI events
    MidiEvent ring_[RING_SIZE];
    alignas(64) std::atomic<uint32_t> ringWrite_{0};
    alignas(64) std::atomic<uint32_t> ringRead_{0};

    // CC callbacks indexed by CC number
    CcCallback ccCallbacks_[128] = {};
    PitchBendCallback pitchBendCallback_;
    RawCallback rawCallback_;

    // Opaque libremidi state (pimpl to avoid exposing libremidi headers)
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace broaudio
