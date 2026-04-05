#include "broaudio/sequencer/sequence.h"
#include "broaudio/synth/voice_allocator.h"

#include <algorithm>

namespace broaudio {

Sequence::Sequence(VoiceAllocator& allocator)
    : allocator_(allocator)
{
}

void Sequence::setBPM(double bpm)
{
    bpm_ = std::max(1.0, std::min(bpm, 999.0));
}

void Sequence::setTimeSignature(int numerator, int denominator)
{
    if (numerator > 0 && denominator > 0) {
        timeSigNumerator_ = numerator;
        timeSigDenominator_ = denominator;
    }
}

void Sequence::addNote(const NoteEvent& event)
{
    notes_.push_back(event);
    sortNotes();
}

void Sequence::removeNote(int index)
{
    if (index >= 0 && index < static_cast<int>(notes_.size())) {
        notes_.erase(notes_.begin() + index);
    }
}

void Sequence::clearNotes()
{
    notes_.clear();
}

void Sequence::play(double engineTime)
{
    playStartEngineTime_ = engineTime;
    pauseOffsetBeats_ = 0.0;
    lastUpdateBeat_ = -1.0;
    playing_ = true;
    paused_ = false;
    activeNotes_.clear();
}

void Sequence::stop()
{
    // Send noteOff for all active notes
    for (auto& an : activeNotes_) {
        allocator_.noteOff(an.note, 0.0);
    }
    activeNotes_.clear();
    playing_ = false;
    paused_ = false;
    lastUpdateBeat_ = -1.0;
    pauseOffsetBeats_ = 0.0;
}

void Sequence::pause(double engineTime)
{
    if (!playing_ || paused_) return;
    pauseOffsetBeats_ = currentBeat(engineTime);
    paused_ = true;
}

void Sequence::resume(double engineTime)
{
    if (!playing_ || !paused_) return;
    playStartEngineTime_ = engineTime;
    paused_ = false;
}

double Sequence::currentBeat(double engineTime) const
{
    if (!playing_) return 0.0;
    if (paused_) return pauseOffsetBeats_;
    return (engineTime - playStartEngineTime_) * bpm_ / 60.0 + pauseOffsetBeats_;
}

double Sequence::beatToEngineTime(double beat) const
{
    return playStartEngineTime_ + (beat - pauseOffsetBeats_) * 60.0 / bpm_;
}

double Sequence::engineTimeToBeat(double engineTime) const
{
    return (engineTime - playStartEngineTime_) * bpm_ / 60.0 + pauseOffsetBeats_;
}

// --- Automation lanes ---

int Sequence::addAutomationLane(AutomationLane::ApplyFn applyFn)
{
    automationLanes_.emplace_back(std::move(applyFn));
    return static_cast<int>(automationLanes_.size()) - 1;
}

void Sequence::removeAutomationLane(int index)
{
    if (index >= 0 && index < static_cast<int>(automationLanes_.size())) {
        automationLanes_.erase(automationLanes_.begin() + index);
    }
}

AutomationLane& Sequence::automationLane(int index)
{
    return automationLanes_[index];
}

const AutomationLane& Sequence::automationLane(int index) const
{
    return automationLanes_[index];
}

void Sequence::clearAutomationLanes()
{
    automationLanes_.clear();
}

// --- Looping ---

void Sequence::setLoopRange(double startBeat, double endBeat)
{
    loopStartBeat_ = std::max(0.0, startBeat);
    loopEndBeat_ = std::max(0.0, endBeat);
}

double Sequence::effectiveLoopEnd() const
{
    if (loopEndBeat_ > loopStartBeat_) return loopEndBeat_;
    // Default: end of sequence (last note end)
    double maxEnd = 0.0;
    for (auto& n : notes_) {
        double end = n.beatPosition + n.duration;
        if (end > maxEnd) maxEnd = end;
    }
    return maxEnd > 0.0 ? maxEnd : 1.0;
}

void Sequence::update(double engineTime)
{
    if (!playing_ || paused_) return;

    double beat = currentBeat(engineTime);

    // Handle loop BEFORE firing notes so we never process beyond the boundary.
    if (loopEnabled_) {
        double loopEnd = effectiveLoopEnd();
        if (beat >= loopEnd) {
            // Release all active notes at loop boundary
            for (auto& an : activeNotes_) {
                allocator_.noteOff(an.note, 0.0);
            }
            activeNotes_.clear();

            // Wrap back to loop start
            pauseOffsetBeats_ = loopStartBeat_;
            playStartEngineTime_ = engineTime;
            lastUpdateBeat_ = loopStartBeat_ - 1e-9;
            return;
        }
    }

    // Fire noteOffs BEFORE noteOns — otherwise a noteOff for a previous note
    // kills a just-started voice when adjacent steps share the same MIDI note.
    for (int i = static_cast<int>(activeNotes_.size()) - 1; i >= 0; i--) {
        if (activeNotes_[i].offBeat <= beat) {
            double offEngTime = beatToEngineTime(activeNotes_[i].offBeat);
            allocator_.noteOff(activeNotes_[i].note, offEngTime);
            activeNotes_.erase(activeNotes_.begin() + i);
        }
    }

    // Fire noteOn for events in (lastUpdateBeat_, beat]
    for (auto& n : notes_) {
        if (n.beatPosition > lastUpdateBeat_ && n.beatPosition <= beat) {
            double noteEngTime = beatToEngineTime(n.beatPosition);
            allocator_.noteOn(n.note, n.velocity, noteEngTime);
            activeNotes_.push_back({n.note, n.beatPosition + n.duration});
        }
    }

    // Apply automation at current beat
    for (auto& lane : automationLanes_) {
        lane.apply(beat);
    }

    lastUpdateBeat_ = beat;
}

void Sequence::sortNotes()
{
    std::sort(notes_.begin(), notes_.end(),
              [](const NoteEvent& a, const NoteEvent& b) {
                  return a.beatPosition < b.beatPosition;
              });
}

} // namespace broaudio
