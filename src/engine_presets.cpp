// Preset application: voice, bus, modulation and whole-engine presets, each
// expressed as calls to the public Engine setters.

#include "broaudio/engine.h"

#include <vector>

namespace broaudio {

// ---------------------------------------------------------------------------
// Preset application
// ---------------------------------------------------------------------------

void Engine::applyVoicePreset(int voiceId, const VoicePreset& p)
{
    setWaveform(voiceId, p.waveform);
    setFrequency(voiceId, p.frequency);
    setGain(voiceId, p.gain);
    setVoicePan(voiceId, p.pan);
    setVoicePitchBend(voiceId, p.pitchBend);
    setAttackTime(voiceId, p.attackTime);
    setDecayTime(voiceId, p.decayTime);
    setSustainLevel(voiceId, p.sustainLevel);
    setReleaseTime(voiceId, p.releaseTime);
    setVoiceFilterEnabled(voiceId, p.filterEnabled);
    setVoiceFilterType(voiceId, p.filterType);
    setVoiceFilterFrequency(voiceId, p.filterFreq);
    setVoiceFilterQ(voiceId, p.filterQ);
    setVoiceUnisonCount(voiceId, p.unisonCount);
    setVoiceUnisonDetune(voiceId, p.unisonDetune);
    setVoiceUnisonStereoWidth(voiceId, p.unisonStereoWidth);
}

void Engine::applyBusPreset(int busId, const BusPreset& p)
{
    setBusGain(busId, p.gain);
    setBusPan(busId, p.pan);

    // Effect order
    int numSlots = static_cast<int>(EffectSlot::Count);
    setBusEffectOrder(busId, p.effectOrder, numSlots);

    // Filters
    for (int i = 0; i < BusPreset::MAX_FILTERS; i++) {
        auto& f = p.filters[i];
        setBusFilterEnabled(busId, i, f.enabled);
        setBusFilterType(busId, i, f.type);
        setBusFilterFrequency(busId, i, f.frequency);
        setBusFilterQ(busId, i, f.Q);
        setBusFilterGain(busId, i, f.gainDB);
    }

    // Delay
    setBusDelayEnabled(busId, p.delay.enabled);
    setBusDelayTime(busId, p.delay.time);
    setBusDelayFeedback(busId, p.delay.feedback);
    setBusDelayMix(busId, p.delay.mix);

    // Compressor
    setBusCompressorEnabled(busId, p.compressor.enabled);
    setBusCompressorThreshold(busId, p.compressor.threshold);
    setBusCompressorRatio(busId, p.compressor.ratio);
    setBusCompressorAttack(busId, p.compressor.attackMs);
    setBusCompressorRelease(busId, p.compressor.releaseMs);

    // Reverb
    setBusReverbEnabled(busId, p.reverb.enabled);
    setBusReverbRoomSize(busId, p.reverb.roomSize);
    setBusReverbDamping(busId, p.reverb.damping);
    setBusReverbMix(busId, p.reverb.mix);

    // Chorus
    setBusChorusEnabled(busId, p.chorus.enabled);
    setBusChorusRate(busId, p.chorus.rate);
    setBusChorusDepth(busId, p.chorus.depth);
    setBusChorusMix(busId, p.chorus.mix);
    setBusChorusFeedback(busId, p.chorus.feedback);
    setBusChorusBaseDelay(busId, p.chorus.baseDelay);

    // Distortion
    setBusDistortionEnabled(busId, p.distortion.enabled);
    setBusDistortionMode(busId, p.distortion.mode);
    setBusDistortionDrive(busId, p.distortion.drive);
    setBusDistortionMix(busId, p.distortion.mix);
    setBusDistortionOutputGain(busId, p.distortion.outputGain);
    setBusDistortionCrushBits(busId, p.distortion.crushBits);
    setBusDistortionCrushRate(busId, p.distortion.crushRate);

    // EQ
    setBusEqEnabled(busId, p.eq.enabled);
    setBusEqMasterGain(busId, p.eq.masterGain);
    for (int i = 0; i < 7; i++)
        setBusEqBandGain(busId, i, p.eq.bandGains[i]);
}

void Engine::applyModPreset(const ModPreset& p)
{
    auto& mm = modMatrix();

    // LFOs
    for (int i = 0; i < ModPreset::MAX_LFOS; i++) {
        mm.setLfoShape(i, p.lfos[i].shape);
        mm.setLfoRate(i, p.lfos[i].rate);
        mm.setLfoDepth(i, p.lfos[i].depth);
        mm.setLfoOffset(i, p.lfos[i].offset);
        mm.setLfoBipolar(i, p.lfos[i].bipolar);
        mm.setLfoSync(i, p.lfos[i].sync);
    }

    // Routes — clear existing and add from preset
    mm.clearAllRoutes();
    for (auto& r : p.routes) {
        int idx = mm.addRoute(r.source, r.dest, r.amount);
        if (idx >= 0)
            mm.setRouteEnabled(idx, r.enabled);
    }
}

void Engine::applyEnginePreset(const EnginePreset& p)
{
    setMasterGain(p.masterGain);
    setLimiterEnabled(p.limiter.enabled);
    setLimiterThreshold(p.limiter.thresholdDb);
    setLimiterRelease(p.limiter.releaseMs);

    // Master bus
    applyBusPreset(MASTER_BUS_ID, p.masterBus);

    // Child buses — we apply to existing buses by index.
    // If the preset has more buses than currently exist, create them.
    // If fewer, extra buses remain as-is.
    auto busList = buses_.load();
    int existingChildren = 0;
    for (auto& b : *busList) {
        if (b->id != MASTER_BUS_ID) existingChildren++;
    }

    // Collect child bus ids
    std::vector<int> childIds;
    for (auto& b : *busList) {
        if (b->id != MASTER_BUS_ID)
            childIds.push_back(b->id);
    }

    for (int i = 0; i < static_cast<int>(p.buses.size()); i++) {
        int busId;
        if (i < static_cast<int>(childIds.size())) {
            busId = childIds[i];
        } else {
            busId = createBus();
        }
        applyBusPreset(busId, p.buses[i]);
    }

    // Modulation
    applyModPreset(p.modulation);
}

} // namespace broaudio
