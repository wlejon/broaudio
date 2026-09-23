#include "broaudio/midi/midi_input.h"
#include "broaudio/engine.h"
#include "broaudio/synth/voice_allocator.h"

#ifdef BROAUDIO_HAS_MIDI
#include <libremidi/libremidi.hpp>
#endif

namespace broaudio {

// ---------------------------------------------------------------------------
// Port-independent half: parsing, the event ring, dispatch. Compiled with or
// without libremidi, so injectMessage works (and is testable) everywhere.
// ---------------------------------------------------------------------------

bool MidiInput::parseMessage(const uint8_t* bytes, size_t size, MidiEvent& event)
{
    if (!bytes || size < 1) return false;

    event = MidiEvent{};
    const uint8_t status = bytes[0] & 0xF0;
    event.channel = bytes[0] & 0x0F;

    switch (status) {
        case 0x90: // Note On
            if (size < 3) return false;
            event.type = MidiEvent::Type::NoteOn;
            event.data1 = bytes[1]; // note
            event.data2 = bytes[2]; // velocity
            if (event.data2 == 0) {
                event.type = MidiEvent::Type::NoteOff; // vel 0 = note off
            }
            return true;
        case 0x80: // Note Off
            if (size < 3) return false;
            event.type = MidiEvent::Type::NoteOff;
            event.data1 = bytes[1];
            event.data2 = bytes[2];
            return true;
        case 0xB0: // Control Change
            if (size < 3) return false;
            event.type = MidiEvent::Type::ControlChange;
            event.data1 = bytes[1]; // CC number
            event.data2 = bytes[2]; // CC value
            return true;
        case 0xE0: // Pitch Bend
            if (size < 3) return false;
            event.type = MidiEvent::Type::PitchBend;
            event.pitchBend = static_cast<int16_t>(
                ((static_cast<int>(bytes[2]) << 7) | bytes[1]) - 8192);
            return true;
        case 0xC0: // Program Change
            if (size < 2) return false;
            event.type = MidiEvent::Type::ProgramChange;
            event.data1 = bytes[1];
            return true;
        case 0xA0: // Polyphonic Aftertouch
            if (size < 3) return false;
            event.type = MidiEvent::Type::Aftertouch;
            event.data1 = bytes[1];
            event.data2 = bytes[2];
            return true;
        case 0xD0: // Channel Pressure
            if (size < 2) return false;
            event.type = MidiEvent::Type::ChannelPressure;
            event.data1 = bytes[1];
            return true;
        default:
            return false; // sysex, clock, etc.
    }
}

bool MidiInput::receiveMessage(const uint8_t* bytes, size_t size, double timestamp)
{
    MidiEvent event;
    if (!parseMessage(bytes, size, event)) return false;
    event.timestamp = timestamp;
    return pushEvent(event);
}

bool MidiInput::injectMessage(const uint8_t* bytes, size_t size, double timestamp)
{
    return receiveMessage(bytes, size, timestamp >= 0.0 ? timestamp : engine_.currentTime());
}

bool MidiInput::pushEvent(const MidiEvent& event)
{
    uint32_t w = ringWrite_.load(std::memory_order_relaxed);
    uint32_t r = ringRead_.load(std::memory_order_acquire);

    // Drop event if ring is full
    if (w - r >= RING_SIZE) return false;

    ring_[w % RING_SIZE] = event;
    ringWrite_.store(w + 1, std::memory_order_release);
    return true;
}

void MidiInput::onControlChange(uint8_t cc, CcCallback fn)
{
    if (cc < 128) ccCallbacks_[cc] = std::move(fn);
}

void MidiInput::processEvents()
{
    uint32_t r = ringRead_.load(std::memory_order_relaxed);
    uint32_t w = ringWrite_.load(std::memory_order_acquire);

    while (r != w) {
        const MidiEvent event = ring_[r % RING_SIZE];

        if (rawCallback_) rawCallback_(event);

        switch (event.type) {
            case MidiEvent::Type::NoteOn:
                if (allocator_) {
                    float vel = static_cast<float>(event.data2) / 127.0f;
                    allocator_->noteOn(event.data1, vel, event.timestamp);
                }
                break;

            case MidiEvent::Type::NoteOff:
                if (allocator_) {
                    allocator_->noteOff(event.data1, event.timestamp);
                }
                break;

            case MidiEvent::Type::ControlChange:
                if (event.data1 < 128 && ccCallbacks_[event.data1]) {
                    ccCallbacks_[event.data1](event.channel, event.data1, event.data2);
                }
                break;

            case MidiEvent::Type::PitchBend:
                if (pitchBendCallback_) {
                    pitchBendCallback_(event.channel, event.pitchBend);
                }
                break;

            default:
                break;
        }

        r++;
    }

    ringRead_.store(r, std::memory_order_release);
}

} // namespace broaudio

#ifdef BROAUDIO_HAS_MIDI

namespace broaudio {

struct MidiInput::Impl {
    libremidi::midi_in input;
    libremidi::observer observer;

    Impl(libremidi::input_configuration inputCfg, libremidi::observer_configuration obsCfg)
        : input(std::move(inputCfg), {})
        , observer(std::move(obsCfg), {})
    {}
};

MidiInput::MidiInput(Engine& engine)
    : engine_(engine)
{
}

MidiInput::~MidiInput()
{
    close();
}

std::vector<MidiPort> MidiInput::availablePorts() const
{
    std::vector<MidiPort> ports;
    try {
        libremidi::observer_configuration obsCfg;
        libremidi::observer obs(std::move(obsCfg), {});
        auto inputs = obs.get_input_ports();
        for (int i = 0; i < static_cast<int>(inputs.size()); i++) {
            ports.push_back({i, inputs[i].port_name});
        }
    } catch (...) {
        // If enumeration fails, return empty list
    }
    return ports;
}

bool MidiInput::open(int portIndex)
{
    close();

    try {
        libremidi::observer_configuration obsCfg;
        libremidi::observer obs(std::move(obsCfg), {});
        auto inputs = obs.get_input_ports();
        if (portIndex < 0 || portIndex >= static_cast<int>(inputs.size())) {
            return false;
        }

        auto port = inputs[portIndex];

        libremidi::input_configuration inputCfg;
        inputCfg.on_message = [this](const libremidi::message& msg) {
            receiveMessage(msg.bytes.data(), msg.bytes.size(), engine_.currentTime());
        };

        libremidi::observer_configuration obsCfg2;
        impl_ = new Impl(std::move(inputCfg), std::move(obsCfg2));
        impl_->input.open_port(port);
        open_ = true;
        return true;

    } catch (...) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
}

void MidiInput::close()
{
    if (impl_) {
        try { impl_->input.close_port(); } catch (...) {}
        delete impl_;
        impl_ = nullptr;
    }
    open_ = false;
}

} // namespace broaudio

#else // !BROAUDIO_HAS_MIDI

// No port backend: enumeration is empty and open() fails, but the event ring
// and injectMessage above still work.
namespace broaudio {

struct MidiInput::Impl {};

MidiInput::MidiInput(Engine& engine) : engine_(engine) {}
MidiInput::~MidiInput() {}
std::vector<MidiPort> MidiInput::availablePorts() const { return {}; }
bool MidiInput::open(int) { return false; }
void MidiInput::close() {}

} // namespace broaudio

#endif // BROAUDIO_HAS_MIDI
