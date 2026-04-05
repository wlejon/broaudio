#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace broaudio {

class Engine;

enum class StealPolicy : uint8_t {
    Oldest,       // steal the voice that started earliest
    Quietest,     // steal the voice with the lowest envelope level
    SameNote,     // steal a voice already playing the same note, else oldest
    None          // don't steal — drop the note if no voice is free
};

// Manages a fixed pool of synth voices with MIDI-style note tracking.
// Handles voice allocation, note-on/off routing, and voice stealing.
// All methods are main-thread only — they call Engine voice APIs internally.
class VoiceAllocator {
public:
    explicit VoiceAllocator(Engine& engine, int maxVoices = 16);
    ~VoiceAllocator();

    VoiceAllocator(const VoiceAllocator&) = delete;
    VoiceAllocator& operator=(const VoiceAllocator&) = delete;

    void setStealPolicy(StealPolicy policy) { stealPolicy_ = policy; }
    StealPolicy stealPolicy() const { return stealPolicy_; }

    void setMaxVoices(int count);
    int maxVoices() const { return maxVoices_; }

    // Note on: allocate a voice for the given MIDI note, return voice id (-1 if dropped)
    // velocity is 0.0-1.0, when is engine time for scheduling
    int noteOn(int note, float velocity, double when = 0.0);

    // Note off: release all voices playing the given note
    void noteOff(int note, double when = 0.0);

    // Release all active voices
    void allNotesOff(double when = 0.0);

    // Voice configuration callback — called after a voice is allocated but before noteOn
    // triggers it. Use this to set waveform, bus routing, ADSR, etc.
    using VoiceSetupFn = std::function<void(int voiceId, int note, float velocity)>;
    void setVoiceSetup(VoiceSetupFn fn) { voiceSetup_ = std::move(fn); }

    // Query
    int activeVoiceCount() const;
    int voiceForNote(int note) const;  // returns first voice id for note, or -1

private:
    struct Slot {
        int voiceId = -1;       // engine voice id
        int note = -1;          // MIDI note currently playing (-1 = idle)
        float velocity = 0.0f;
        double startTime = 0.0; // when this note started
        bool active = false;    // currently sounding (not yet released)
    };

    int findFreeSlot() const;
    int findStealSlot(int note) const;

    Engine& engine_;
    int maxVoices_;
    StealPolicy stealPolicy_ = StealPolicy::Oldest;
    std::vector<Slot> slots_;
    VoiceSetupFn voiceSetup_;
};

} // namespace broaudio
