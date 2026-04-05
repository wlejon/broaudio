#include "broaudio/synth/voice_allocator.h"
#include "broaudio/engine.h"

#include <algorithm>
#include <limits>

namespace broaudio {

VoiceAllocator::VoiceAllocator(Engine& engine, int maxVoices)
    : engine_(engine)
    , maxVoices_(maxVoices)
{
    slots_.resize(maxVoices);
    for (auto& slot : slots_) {
        slot.voiceId = engine_.createVoice();
        engine_.setVoicePersistent(slot.voiceId, true);
    }
}

VoiceAllocator::~VoiceAllocator()
{
    for (auto& slot : slots_) {
        if (slot.voiceId >= 0)
            engine_.removeVoice(slot.voiceId);
    }
}

void VoiceAllocator::setMaxVoices(int count)
{
    if (count == maxVoices_) return;

    if (count > maxVoices_) {
        // Add new slots
        slots_.resize(count);
        for (int i = maxVoices_; i < count; i++) {
            slots_[i].voiceId = engine_.createVoice();
            engine_.setVoicePersistent(slots_[i].voiceId, true);
        }
    } else {
        // Remove excess slots (release active ones first)
        for (int i = count; i < maxVoices_; i++) {
            if (slots_[i].active) {
                engine_.stopVoice(slots_[i].voiceId, 0.0);
            }
            engine_.removeVoice(slots_[i].voiceId);
        }
        slots_.resize(count);
    }
    maxVoices_ = count;
}

int VoiceAllocator::findFreeSlot() const
{
    for (int i = 0; i < maxVoices_; i++) {
        if (!slots_[i].active) return i;
    }
    return -1;
}

int VoiceAllocator::findStealSlot(int note) const
{
    switch (stealPolicy_) {
        case StealPolicy::None:
            return -1;

        case StealPolicy::SameNote: {
            // First try to find a voice playing the same note
            for (int i = 0; i < maxVoices_; i++) {
                if (slots_[i].active && slots_[i].note == note) return i;
            }
            // Fall through to oldest
            [[fallthrough]];
        }

        case StealPolicy::Oldest: {
            int oldest = -1;
            double earliestTime = std::numeric_limits<double>::max();
            for (int i = 0; i < maxVoices_; i++) {
                if (slots_[i].active && slots_[i].startTime < earliestTime) {
                    earliestTime = slots_[i].startTime;
                    oldest = i;
                }
            }
            return oldest;
        }

        case StealPolicy::Quietest: {
            // We can't read envelope level from here (audio-thread state),
            // so use velocity as a proxy — lowest velocity = quietest
            int quietest = -1;
            float lowestVel = std::numeric_limits<float>::max();
            for (int i = 0; i < maxVoices_; i++) {
                if (slots_[i].active && slots_[i].velocity < lowestVel) {
                    lowestVel = slots_[i].velocity;
                    quietest = i;
                }
            }
            return quietest;
        }
    }
    return -1;
}

int VoiceAllocator::noteOn(int note, float velocity, double when)
{
    double time = when > 0.0 ? when : engine_.currentTime();

    int slotIdx = findFreeSlot();
    if (slotIdx < 0) {
        slotIdx = findStealSlot(note);
        if (slotIdx < 0) return -1;

        // Stop the stolen voice immediately
        engine_.stopVoice(slots_[slotIdx].voiceId, time);

        // Re-create the voice to get fresh audio state
        engine_.removeVoice(slots_[slotIdx].voiceId);
        slots_[slotIdx].voiceId = engine_.createVoice();
        engine_.setVoicePersistent(slots_[slotIdx].voiceId, true);
    }

    Slot& slot = slots_[slotIdx];
    slot.note = note;
    slot.velocity = velocity;
    slot.startTime = time;
    slot.active = true;

    // Initialize per-voice modulation state (note number, velocity, LFO phases)
    engine_.setVoiceNote(slot.voiceId, note, velocity);

    // Let the user configure the voice (waveform, bus, ADSR, etc.)
    if (voiceSetup_) {
        voiceSetup_(slot.voiceId, note, velocity);
    }

    // Set gain from velocity and start
    engine_.setGain(slot.voiceId, velocity);
    engine_.startVoice(slot.voiceId, time);

    return slot.voiceId;
}

void VoiceAllocator::noteOff(int note, double when)
{
    double time = when > 0.0 ? when : engine_.currentTime();

    for (auto& slot : slots_) {
        if (slot.active && slot.note == note) {
            engine_.stopVoice(slot.voiceId, time);
            slot.active = false;
            slot.note = -1;
        }
    }
}

void VoiceAllocator::allNotesOff(double when)
{
    double time = when > 0.0 ? when : engine_.currentTime();

    for (auto& slot : slots_) {
        if (slot.active) {
            engine_.stopVoice(slot.voiceId, time);
            slot.active = false;
            slot.note = -1;
        }
    }
}

int VoiceAllocator::activeVoiceCount() const
{
    int count = 0;
    for (auto& slot : slots_) {
        if (slot.active) count++;
    }
    return count;
}

int VoiceAllocator::voiceForNote(int note) const
{
    for (auto& slot : slots_) {
        if (slot.active && slot.note == note) return slot.voiceId;
    }
    return -1;
}

} // namespace broaudio
