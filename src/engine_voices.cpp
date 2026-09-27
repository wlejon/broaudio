// Synth voices (create/remove, oscillator, envelope, filter, unison), the
// legacy master-bus shortcuts, master gain/limiter, and the listener, Doppler,
// head model and per-source spatial setters. Control thread only.

#include "broaudio/engine.h"
#include "broaudio/synth/wavetable.h"

#include <cmath>
#include <algorithm>

namespace broaudio {

static constexpr float DEFAULT_ATTACK  = 0.01f;
static constexpr float DEFAULT_DECAY   = 0.1f;
static constexpr float DEFAULT_SUSTAIN = 1.0f;
static constexpr float DEFAULT_RELEASE = 0.04f;

// ---------------------------------------------------------------------------
// Master bus shortcuts — delegate to per-bus methods
// ---------------------------------------------------------------------------

int Engine::allocateFilterSlot() { return allocateBusFilterSlot(MASTER_BUS_ID); }
void Engine::releaseFilterSlot(int s) { releaseBusFilterSlot(MASTER_BUS_ID, s); }
void Engine::setFilterEnabled(int s, bool e) { setBusFilterEnabled(MASTER_BUS_ID, s, e); }
void Engine::setFilterType(int s, BiquadFilter::Type t) { setBusFilterType(MASTER_BUS_ID, s, t); }
void Engine::setFilterFrequency(int s, float f) { setBusFilterFrequency(MASTER_BUS_ID, s, f); }
void Engine::setFilterQ(int s, float q) { setBusFilterQ(MASTER_BUS_ID, s, q); }
void Engine::setFilterGain(int s, float g) { setBusFilterGain(MASTER_BUS_ID, s, g); }

void Engine::setDelayEnabled(bool e) { setBusDelayEnabled(MASTER_BUS_ID, e); }
void Engine::setDelayTime(float s) { setBusDelayTime(MASTER_BUS_ID, s); }
void Engine::setDelayFeedback(float f) { setBusDelayFeedback(MASTER_BUS_ID, f); }
void Engine::setDelayMix(float m) { setBusDelayMix(MASTER_BUS_ID, m); }

// ---------------------------------------------------------------------------
// Voice management — RCU for the list, atomics for parameters
// ---------------------------------------------------------------------------

Voice* Engine::findVoice(int id)
{
    auto currentVoices = voices_.load();
    for (auto& v : *currentVoices) {
        if (v->id == id) return v.get();
    }
    return nullptr;
}

int Engine::createVoice()
{
    std::lock_guard<std::mutex> lock(voiceWriteMutex_);

    auto voice = std::make_shared<Voice>();
    voice->id = nextVoiceId_++;
    // Bind the wavetable RCU slot to the engine's domain before publishing
    // the voice — once the voice is in voices_ the audio thread may load
    // wavetable, and any later setVoiceWavetable() needs a domain to retire
    // old snapshots into.
    voice->wavetable.setDomain(rcu_);
    float sr = static_cast<float>(sampleRate_);
    voice->attackRate.store(1.0f / (DEFAULT_ATTACK * sr), std::memory_order_relaxed);
    voice->decayCoeff.store(std::exp(-3.0f / (DEFAULT_DECAY * sr)), std::memory_order_relaxed);
    voice->sustainLevel.store(DEFAULT_SUSTAIN, std::memory_order_relaxed);
    voice->releaseCoeff.store(std::exp(-3.0f / (DEFAULT_RELEASE * sr)), std::memory_order_relaxed);

    // Build new list, dropping any one-shot voices that have finished —
    // this amortizes the cleanup the audio thread used to do, so hosts that
    // never call update() still don't accumulate dead voices indefinitely.
    auto current = voices_.load();
    auto newList = std::make_shared<VoiceList>();
    newList->reserve(current->size() + 1);
    for (auto& v : *current) {
        bool finished = v->started && (v->envStage == EnvStage::Done || !v->active);
        bool persistent = v->persistent.load(std::memory_order_relaxed);
        if (!finished || persistent) newList->push_back(v);
    }
    newList->push_back(voice);
    voices_.store(std::move(newList));
    voiceCleanupNeeded_.store(false, std::memory_order_relaxed);

    return voice->id;
}

void Engine::removeVoice(int id)
{
    std::lock_guard<std::mutex> lock(voiceWriteMutex_);

    auto current = voices_.load();
    auto newList = std::make_shared<VoiceList>();
    newList->reserve(current->size());
    for (auto& v : *current) {
        if (v->id != id) newList->push_back(v);
    }
    voices_.store(std::move(newList));
}

void Engine::setWaveform(int id, Waveform wf)
{
    if (auto* v = findVoice(id))
        v->waveform.store(wf, std::memory_order_relaxed);
}

void Engine::setVoiceWavetable(int id, std::shared_ptr<const WavetableBank> bank)
{
    if (auto* v = findVoice(id)) {
        v->waveform.store(Waveform::Wavetable, std::memory_order_relaxed);
        v->wavetable.store(std::move(bank), std::memory_order_release);
    }
}

void Engine::setFrequency(int id, float freq)
{
    if (auto* v = findVoice(id))
        v->frequency.store(freq, std::memory_order_relaxed);
}

void Engine::setGain(int id, float gain)
{
    if (auto* v = findVoice(id))
        v->gain.store(gain, std::memory_order_relaxed);
}

void Engine::setVoicePan(int id, float pan)
{
    if (auto* v = findVoice(id))
        v->pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void Engine::setVoicePitchBend(int id, float semitones)
{
    if (auto* v = findVoice(id))
        v->pitchBend.store(semitones, std::memory_order_relaxed);
}

void Engine::setMasterGain(float gain)
{
    masterGain_.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
}

void Engine::setLimiterEnabled(bool enabled)
{
    masterLimiter_.setEnabled(enabled);
}

void Engine::setLimiterThreshold(float thresholdDb)
{
    masterLimiter_.setThreshold(thresholdDb);
}

void Engine::setLimiterRelease(float releaseMs)
{
    masterLimiter_.setRelease(releaseMs);
}

void Engine::setAttackTime(int id, float seconds)
{
    if (auto* v = findVoice(id))
        v->attackRate.store(seconds > 0.0001f ? 1.0f / (seconds * static_cast<float>(sampleRate_)) : 1.0f,
                           std::memory_order_relaxed);
}

void Engine::setDecayTime(int id, float seconds)
{
    if (auto* v = findVoice(id))
        v->decayCoeff.store(seconds > 0.0001f
            ? std::exp(-3.0f / (seconds * static_cast<float>(sampleRate_)))
            : 0.0f, std::memory_order_relaxed);
}

void Engine::setSustainLevel(int id, float level)
{
    if (auto* v = findVoice(id))
        v->sustainLevel.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Engine::setReleaseTime(int id, float seconds)
{
    if (auto* v = findVoice(id))
        v->releaseCoeff.store(seconds > 0.0001f
            ? std::exp(-3.0f / (seconds * static_cast<float>(sampleRate_)))
            : 0.0f, std::memory_order_relaxed);
}

void Engine::setVoiceFilterEnabled(int id, bool enabled)
{
    if (auto* v = findVoice(id)) {
        v->filterEnabled.store(enabled, std::memory_order_relaxed);
        v->filterVersion.fetch_add(1, std::memory_order_release);
    }
}

void Engine::setVoiceFilterType(int id, BiquadFilter::Type type)
{
    if (auto* v = findVoice(id)) {
        v->filterType.store(static_cast<int>(type), std::memory_order_relaxed);
        v->filterVersion.fetch_add(1, std::memory_order_release);
    }
}

void Engine::setVoiceFilterFrequency(int id, float freq)
{
    if (auto* v = findVoice(id)) {
        v->filterFrequency.store(std::clamp(freq, 20.0f, 20000.0f), std::memory_order_relaxed);
        v->filterVersion.fetch_add(1, std::memory_order_release);
    }
}

void Engine::setVoiceFilterQ(int id, float q)
{
    if (auto* v = findVoice(id)) {
        v->filterQ.store(std::clamp(q, 0.1f, 30.0f), std::memory_order_relaxed);
        v->filterVersion.fetch_add(1, std::memory_order_release);
    }
}

void Engine::setVoiceUnisonCount(int id, int count)
{
    if (auto* v = findVoice(id)) {
        v->unisonCount.store(std::clamp(count, 1, Voice::MAX_UNISON), std::memory_order_relaxed);
        v->unisonVersion.fetch_add(1, std::memory_order_release);
    }
}

void Engine::setVoiceUnisonDetune(int id, float semitones)
{
    if (auto* v = findVoice(id)) {
        v->unisonDetune.store(std::clamp(semitones, 0.0f, 2.0f), std::memory_order_relaxed);
        v->unisonVersion.fetch_add(1, std::memory_order_release);
    }
}

void Engine::setVoiceUnisonStereoWidth(int id, float width)
{
    if (auto* v = findVoice(id)) {
        v->unisonStereoWidth.store(std::clamp(width, 0.0f, 1.0f), std::memory_order_relaxed);
        v->unisonVersion.fetch_add(1, std::memory_order_release);
    }
}

void Engine::setVoiceNote(int id, int noteNumber, float velocity)
{
    if (auto* v = findVoice(id)) {
        v->modState.reset(noteNumber, velocity);
        v->modState.resetSyncedPhases(modMatrix_.lfoParamsArray());
    }
}

void Engine::setVoicePersistent(int id, bool persistent)
{
    if (auto* v = findVoice(id)) {
        v->persistent.store(persistent, std::memory_order_relaxed);
    }
}

void Engine::startVoice(int id, double when)
{
    if (auto* v = findVoice(id)) v->markStart(when);
}

void Engine::stopVoice(int id, double when)
{
    if (when > currentTime()) {
        scheduleNoteOff(id, when);
    } else if (auto* v = findVoice(id)) {
        v->markRelease();
    }
}

// ---------------------------------------------------------------------------
// Spatial
// ---------------------------------------------------------------------------

void Engine::setListenerPosition(float x, float y, float z)
{
    listener_.posX.store(x, std::memory_order_relaxed);
    listener_.posY.store(y, std::memory_order_relaxed);
    listener_.posZ.store(z, std::memory_order_relaxed);
}

void Engine::setListenerOrientation(float fx, float fy, float fz,
                                     float ux, float uy, float uz)
{
    listener_.fwdX.store(fx, std::memory_order_relaxed);
    listener_.fwdY.store(fy, std::memory_order_relaxed);
    listener_.fwdZ.store(fz, std::memory_order_relaxed);
    listener_.upX.store(ux, std::memory_order_relaxed);
    listener_.upY.store(uy, std::memory_order_relaxed);
    listener_.upZ.store(uz, std::memory_order_relaxed);
}

void Engine::setListenerVelocity(float x, float y, float z)
{
    listener_.velX.store(x, std::memory_order_relaxed);
    listener_.velY.store(y, std::memory_order_relaxed);
    listener_.velZ.store(z, std::memory_order_relaxed);
}

void Engine::setDopplerFactor(float factor)
{
    dopplerFactor_.store(std::max(0.0f, factor), std::memory_order_relaxed);
}

float Engine::getPlaybackDopplerRatio(int instanceId) const
{
    if (auto* pb = findPlayback(instanceId))
        return pb->spatial.lastDopplerRatio.load(std::memory_order_relaxed);
    return 1.0f;
}

float Engine::getVoiceDopplerRatio(int voiceId) const
{
    auto currentVoices = voices_.load();
    for (auto& v : *currentVoices) {
        if (v->id == voiceId)
            return v->spatial.lastDopplerRatio.load(std::memory_order_relaxed);
    }
    return 1.0f;
}

// --- Head model ---

void Engine::setHeadModelEnabled(bool enabled) { headModel_.enabled.store(enabled, std::memory_order_relaxed); }
void Engine::setHeadModelIldStrength(float s) { headModel_.ildStrength.store(s, std::memory_order_relaxed); }
void Engine::setHeadModelBehindAttenuation(float a) { headModel_.behindAttenuation.store(a, std::memory_order_relaxed); }
void Engine::setHeadModelNearCutoff(float front, float behind) {
    headModel_.nearCutoffFront.store(front, std::memory_order_relaxed);
    headModel_.nearCutoffBehind.store(behind, std::memory_order_relaxed);
}
void Engine::setHeadModelFarCutoffRatio(float r) { headModel_.farCutoffRatio.store(r, std::memory_order_relaxed); }
void Engine::setHeadModelElevation(float nearHz, float farHz) {
    headModel_.elevationNear.store(nearHz, std::memory_order_relaxed);
    headModel_.elevationFar.store(farHz, std::memory_order_relaxed);
}
void Engine::setHeadModelCutoffRange(float minHz, float maxHz) {
    headModel_.minCutoff.store(minHz, std::memory_order_relaxed);
    headModel_.maxCutoff.store(maxHz, std::memory_order_relaxed);
}

// --- Spatial sources (voice) ---

static void setSpatialEnabled(SpatialSource& s, bool enabled) { s.spatialEnabled.store(enabled, std::memory_order_relaxed); }
static void setSpatialPosition(SpatialSource& s, float x, float y, float z) {
    s.posX.store(x, std::memory_order_relaxed);
    s.posY.store(y, std::memory_order_relaxed);
    s.posZ.store(z, std::memory_order_relaxed);
}
static void setSpatialVelocity(SpatialSource& s, float x, float y, float z) {
    s.velX.store(x, std::memory_order_relaxed);
    s.velY.store(y, std::memory_order_relaxed);
    s.velZ.store(z, std::memory_order_relaxed);
}
static void setSpatialRefDistance(SpatialSource& s, float d) { s.refDistance.store(std::max(0.001f, d), std::memory_order_relaxed); }
static void setSpatialMaxDistance(SpatialSource& s, float d) { s.maxDistance.store(std::max(0.001f, d), std::memory_order_relaxed); }
static void setSpatialRolloff(SpatialSource& s, float r) { s.rolloff.store(std::max(0.0f, r), std::memory_order_relaxed); }
static void setSpatialDistanceModel(SpatialSource& s, DistanceModel m) { s.distanceModel.store(static_cast<int>(m), std::memory_order_relaxed); }
static void setSpatialOcclusion(SpatialSource& s, float occ) { s.occlusion.store(std::clamp(occ, 0.0f, 1.0f), std::memory_order_relaxed); }

void Engine::setVoiceSpatialEnabled(int id, bool enabled) { if (auto* v = findVoice(id)) setSpatialEnabled(v->spatial, enabled); }
void Engine::setVoiceSpatialPosition(int id, float x, float y, float z) { if (auto* v = findVoice(id)) setSpatialPosition(v->spatial, x, y, z); }
void Engine::setVoiceSpatialVelocity(int id, float x, float y, float z) { if (auto* v = findVoice(id)) setSpatialVelocity(v->spatial, x, y, z); }
void Engine::setVoiceSpatialRefDistance(int id, float d) { if (auto* v = findVoice(id)) setSpatialRefDistance(v->spatial, d); }
void Engine::setVoiceSpatialMaxDistance(int id, float d) { if (auto* v = findVoice(id)) setSpatialMaxDistance(v->spatial, d); }
void Engine::setVoiceSpatialRolloff(int id, float r) { if (auto* v = findVoice(id)) setSpatialRolloff(v->spatial, r); }
void Engine::setVoiceSpatialDistanceModel(int id, DistanceModel m) { if (auto* v = findVoice(id)) setSpatialDistanceModel(v->spatial, m); }
void Engine::setVoiceSpatialOcclusion(int id, float occ) { if (auto* v = findVoice(id)) setSpatialOcclusion(v->spatial, occ); }

void Engine::setPlaybackSpatialEnabled(int id, bool enabled) { if (auto* pb = findPlayback(id)) setSpatialEnabled(pb->spatial, enabled); }
void Engine::setPlaybackSpatialPosition(int id, float x, float y, float z) { if (auto* pb = findPlayback(id)) setSpatialPosition(pb->spatial, x, y, z); }
void Engine::setPlaybackSpatialVelocity(int id, float x, float y, float z) { if (auto* pb = findPlayback(id)) setSpatialVelocity(pb->spatial, x, y, z); }
void Engine::setPlaybackSpatialRefDistance(int id, float d) { if (auto* pb = findPlayback(id)) setSpatialRefDistance(pb->spatial, d); }
void Engine::setPlaybackSpatialMaxDistance(int id, float d) { if (auto* pb = findPlayback(id)) setSpatialMaxDistance(pb->spatial, d); }
void Engine::setPlaybackSpatialRolloff(int id, float r) { if (auto* pb = findPlayback(id)) setSpatialRolloff(pb->spatial, r); }
void Engine::setPlaybackSpatialDistanceModel(int id, DistanceModel m) { if (auto* pb = findPlayback(id)) setSpatialDistanceModel(pb->spatial, m); }
void Engine::setPlaybackSpatialOcclusion(int id, float occ) { if (auto* pb = findPlayback(id)) setSpatialOcclusion(pb->spatial, occ); }

} // namespace broaudio
