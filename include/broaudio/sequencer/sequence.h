#pragma once

#include <cstdint>
#include <vector>

namespace broaudio {

class VoiceAllocator;

struct NoteEvent {
    double beatPosition;  // beat number (0-based, fractional ok)
    int note;             // MIDI note number
    float velocity;       // 0-1
    double duration;      // length in beats
};

class Sequence {
public:
    explicit Sequence(VoiceAllocator& allocator);

    // Tempo
    void setBPM(double bpm);
    double bpm() const { return bpm_; }

    // Time signature (informational — beat timing is always quarter-note based)
    void setTimeSignature(int numerator, int denominator);
    int timeSignatureNumerator() const { return timeSigNumerator_; }
    int timeSignatureDenominator() const { return timeSigDenominator_; }

    // Event management
    void addNote(const NoteEvent& event);
    void removeNote(int index);
    void clearNotes();
    int noteCount() const { return static_cast<int>(notes_.size()); }
    const NoteEvent& note(int index) const { return notes_[index]; }

    // Transport
    void play(double engineTime);
    void stop();
    void pause(double engineTime);
    void resume(double engineTime);
    bool isPlaying() const { return playing_; }
    bool isPaused() const { return paused_; }

    // Looping
    void setLoopEnabled(bool enabled) { loopEnabled_ = enabled; }
    void setLoopRange(double startBeat, double endBeat);
    bool isLoopEnabled() const { return loopEnabled_; }
    double loopStartBeat() const { return loopStartBeat_; }
    double loopEndBeat() const { return loopEndBeat_; }

    // Query
    double currentBeat(double engineTime) const;
    double beatToEngineTime(double beat) const;
    double engineTimeToBeat(double engineTime) const;

    // Call this regularly from the main thread (e.g., each frame).
    // Drains pending beat-position events and fires noteOn/noteOff
    // on the VoiceAllocator at the correct engine time.
    void update(double engineTime);

private:
    VoiceAllocator& allocator_;

    // Tempo
    double bpm_ = 120.0;
    int timeSigNumerator_ = 4;
    int timeSigDenominator_ = 4;

    // Events sorted by beat position
    std::vector<NoteEvent> notes_;

    // Transport state
    bool playing_ = false;
    bool paused_ = false;
    double playStartEngineTime_ = 0.0;
    double pauseOffsetBeats_ = 0.0;

    // Playback cursor
    double lastUpdateBeat_ = 0.0;

    // Looping
    bool loopEnabled_ = false;
    double loopStartBeat_ = 0.0;
    double loopEndBeat_ = 0.0;  // 0 = end of sequence

    // Active note tracking for noteOff scheduling
    struct ActiveNote {
        int note;
        double offBeat;
    };
    std::vector<ActiveNote> activeNotes_;

    double effectiveLoopEnd() const;
    void sortNotes();
};

} // namespace broaudio
