// Per-bus effect parameters (delay, compressor, reverb, chorus, distortion,
// EQ, JIT pipeline), metering reads, event scheduling, effect order and
// voice/playback/bus routing and sends. Control thread only.

#include "broaudio/engine.h"
#include "broaudio/dsp/equalizer.h"

#include <algorithm>

namespace broaudio {

// --- Per-bus delay control ---

void Engine::setBusDelayEnabled(int busId, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->delayParams.enabled.store(enabled, std::memory_order_relaxed);
    bus->delayParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusDelayTime(int busId, float seconds)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->delayParams.time.store(std::clamp(seconds, 0.001f, 2.0f), std::memory_order_relaxed);
    bus->delayParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusDelayFeedback(int busId, float fb)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->delayParams.feedback.store(std::clamp(fb, 0.0f, 0.95f), std::memory_order_relaxed);
    bus->delayParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusDelayMix(int busId, float mix)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->delayParams.mix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
    bus->delayParams.version.fetch_add(1, std::memory_order_release);
}

bool Engine::getBusDelayEnabled(int busId) const {
    if (auto* bus = findBus(busId)) return bus->delayParams.enabled.load(std::memory_order_relaxed);
    return false;
}

float Engine::getBusDelayTime(int busId) const {
    if (auto* bus = findBus(busId)) return bus->delayParams.time.load(std::memory_order_relaxed);
    return 0.3f;
}

float Engine::getBusDelayFeedback(int busId) const {
    if (auto* bus = findBus(busId)) return bus->delayParams.feedback.load(std::memory_order_relaxed);
    return 0.3f;
}

float Engine::getBusDelayMix(int busId) const {
    if (auto* bus = findBus(busId)) return bus->delayParams.mix.load(std::memory_order_relaxed);
    return 0.5f;
}

// --- Per-bus compressor control ---

void Engine::setBusCompressorEnabled(int busId, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->compressorParams.enabled.store(enabled, std::memory_order_relaxed);
    bus->compressorParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusCompressorThreshold(int busId, float threshold)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->compressorParams.threshold.store(std::clamp(threshold, 0.0f, 1.0f), std::memory_order_relaxed);
    bus->compressorParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusCompressorRatio(int busId, float ratio)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->compressorParams.ratio.store(std::clamp(ratio, 1.0f, 20.0f), std::memory_order_relaxed);
    bus->compressorParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusCompressorAttack(int busId, float ms)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->compressorParams.attackMs.store(std::clamp(ms, 0.1f, 100.0f), std::memory_order_relaxed);
    bus->compressorParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusCompressorRelease(int busId, float ms)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->compressorParams.releaseMs.store(std::clamp(ms, 1.0f, 1000.0f), std::memory_order_relaxed);
    bus->compressorParams.version.fetch_add(1, std::memory_order_release);
}

bool Engine::getBusCompressorEnabled(int busId) const {
    if (auto* bus = findBus(busId)) return bus->compressorParams.enabled.load(std::memory_order_relaxed);
    return false;
}

float Engine::getBusCompressorThreshold(int busId) const {
    if (auto* bus = findBus(busId)) return bus->compressorParams.threshold.load(std::memory_order_relaxed);
    return 0.7f;
}

float Engine::getBusCompressorRatio(int busId) const {
    if (auto* bus = findBus(busId)) return bus->compressorParams.ratio.load(std::memory_order_relaxed);
    return 4.0f;
}

float Engine::getBusCompressorAttack(int busId) const {
    if (auto* bus = findBus(busId)) return bus->compressorParams.attackMs.load(std::memory_order_relaxed);
    return 1.0f;
}

float Engine::getBusCompressorRelease(int busId) const {
    if (auto* bus = findBus(busId)) return bus->compressorParams.releaseMs.load(std::memory_order_relaxed);
    return 100.0f;
}

int Engine::getBusCompressorSidechain(int busId) const {
    if (auto* bus = findBus(busId)) return bus->compressorParams.sidechainBusId.load(std::memory_order_relaxed);
    return -1;
}

// --- Per-bus reverb control ---

void Engine::setBusReverbEnabled(int busId, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->reverbParams.enabled.store(enabled, std::memory_order_relaxed);
    bus->reverbParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusReverbRoomSize(int busId, float size)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->reverbParams.roomSize.store(std::clamp(size, 0.0f, 1.0f), std::memory_order_relaxed);
    bus->reverbParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusReverbDamping(int busId, float damping)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->reverbParams.damping.store(std::clamp(damping, 0.0f, 1.0f), std::memory_order_relaxed);
    bus->reverbParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusReverbMix(int busId, float mix)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->reverbParams.mix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
    bus->reverbParams.version.fetch_add(1, std::memory_order_release);
}

// --- Per-bus chorus/flanger control ---

void Engine::setBusChorusEnabled(int busId, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->chorusParams.enabled.store(enabled, std::memory_order_relaxed);
    bus->chorusParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusChorusRate(int busId, float hz)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->chorusParams.rate.store(std::clamp(hz, 0.01f, 20.0f), std::memory_order_relaxed);
    bus->chorusParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusChorusDepth(int busId, float seconds)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->chorusParams.depth.store(std::clamp(seconds, 0.0001f, 0.05f), std::memory_order_relaxed);
    bus->chorusParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusChorusMix(int busId, float mix)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->chorusParams.mix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
    bus->chorusParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusChorusFeedback(int busId, float fb)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->chorusParams.feedback.store(std::clamp(fb, 0.0f, 0.95f), std::memory_order_relaxed);
    bus->chorusParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusChorusBaseDelay(int busId, float seconds)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->chorusParams.baseDelay.store(std::clamp(seconds, 0.001f, 0.05f), std::memory_order_relaxed);
    bus->chorusParams.version.fetch_add(1, std::memory_order_release);
}

// --- Per-bus distortion/waveshaper control ---

void Engine::setBusDistortionEnabled(int busId, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.enabled.store(enabled, std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
    syncBusJitTopology(*bus);
}

void Engine::setBusDistortionMode(int busId, DistortionMode mode)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.mode.store(static_cast<int>(mode), std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
    syncBusJitTopology(*bus);
}

void Engine::setBusDistortionDrive(int busId, float drive)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.drive.store(std::clamp(drive, 0.1f, 100.0f), std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusDistortionMix(int busId, float mix)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.mix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusDistortionOutputGain(int busId, float gain)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.outputGain.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusDistortionCrushBits(int busId, float bits)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.crushBits.store(std::clamp(bits, 1.0f, 16.0f), std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusDistortionCrushRate(int busId, float rate)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.crushRate.store(std::clamp(rate, 0.01f, 1.0f), std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
}

// --- Per-bus JIT pipeline control ---

void Engine::setBusJitEnabled(int busId, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->jitEnabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        syncBusJitTopology(*bus);
    }
}

bool Engine::isBusJitEnabled(int busId) const
{
    auto* bus = findBus(busId);
    return bus ? bus->jitEnabled.load(std::memory_order_relaxed) : false;
}

bool Engine::isBusJitActive(int busId) const
{
    auto* bus = findBus(busId);
    return bus ? bus->jitActive.load(std::memory_order_relaxed) : false;
}

void Engine::syncBusJitTopology(Bus& bus)
{
    if (!bus.jitEnabled.load(std::memory_order_relaxed)) return;

    JitTopology topo;
    int count = 0;
    for (int f = 0; f < Bus::MAX_FILTERS; f++) {
        if (bus.filterParams[f].enabled.load(std::memory_order_relaxed)) {
            count++;
        }
    }
    topo.filterCount = count;
    topo.hasDistortion = bus.distortionParams.enabled.load(std::memory_order_relaxed);
    topo.distortionMode = static_cast<DistortionMode>(bus.distortionParams.mode.load(std::memory_order_relaxed));

    if (topo == bus.currentTopology && bus.jitPipeline.load(std::memory_order_relaxed) != nullptr) {
        return;
    }

    bus.currentTopology = topo;
    int busId = bus.id;

    jitCompiler_.compileAsync(topo, [this, busId](std::shared_ptr<JitBusPipeline> pipeline) {
        auto currentBuses = buses_.load();
        for (auto& b : *currentBuses) {
            if (b->id == busId) {
                std::lock_guard<std::mutex> lock(b->jitMutex);
                b->jitPipeline.store(pipeline);
                break;
            }
        }
    });
}

// --- Per-bus equalizer control ---

void Engine::setBusEqEnabled(int busId, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->eqParams.enabled.store(enabled, std::memory_order_relaxed);
    bus->eqParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusEqBandGain(int busId, int band, float gainDB)
{
    auto* bus = findBus(busId);
    if (!bus || band < 0 || band >= Equalizer::NUM_BANDS) return;
    bus->eqParams.bandGains[band].store(std::clamp(gainDB, -12.0f, 12.0f), std::memory_order_relaxed);
    bus->eqParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusEqMasterGain(int busId, float gainDB)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->eqParams.masterGain.store(std::clamp(gainDB, 0.0f, 11.0f), std::memory_order_relaxed);
    bus->eqParams.version.fetch_add(1, std::memory_order_release);
}

bool Engine::getBusReverbEnabled(int busId) const {
    if (auto* bus = findBus(busId)) return bus->reverbParams.enabled.load(std::memory_order_relaxed);
    return false;
}

float Engine::getBusReverbRoomSize(int busId) const {
    if (auto* bus = findBus(busId)) return bus->reverbParams.roomSize.load(std::memory_order_relaxed);
    return 0.85f;
}

float Engine::getBusReverbDamping(int busId) const {
    if (auto* bus = findBus(busId)) return bus->reverbParams.damping.load(std::memory_order_relaxed);
    return 0.5f;
}

float Engine::getBusReverbMix(int busId) const {
    if (auto* bus = findBus(busId)) return bus->reverbParams.mix.load(std::memory_order_relaxed);
    return 0.3f;
}

bool Engine::getBusChorusEnabled(int busId) const {
    if (auto* bus = findBus(busId)) return bus->chorusParams.enabled.load(std::memory_order_relaxed);
    return false;
}

float Engine::getBusChorusRate(int busId) const {
    if (auto* bus = findBus(busId)) return bus->chorusParams.rate.load(std::memory_order_relaxed);
    return 0.5f;
}

float Engine::getBusChorusDepth(int busId) const {
    if (auto* bus = findBus(busId)) return bus->chorusParams.depth.load(std::memory_order_relaxed);
    return 0.005f;
}

float Engine::getBusChorusMix(int busId) const {
    if (auto* bus = findBus(busId)) return bus->chorusParams.mix.load(std::memory_order_relaxed);
    return 0.5f;
}

float Engine::getBusChorusFeedback(int busId) const {
    if (auto* bus = findBus(busId)) return bus->chorusParams.feedback.load(std::memory_order_relaxed);
    return 0.0f;
}

float Engine::getBusChorusBaseDelay(int busId) const {
    if (auto* bus = findBus(busId)) return bus->chorusParams.baseDelay.load(std::memory_order_relaxed);
    return 0.01f;
}

bool Engine::getBusDistortionEnabled(int busId) const {
    if (auto* bus = findBus(busId)) return bus->distortionParams.enabled.load(std::memory_order_relaxed);
    return false;
}

DistortionMode Engine::getBusDistortionMode(int busId) const {
    if (auto* bus = findBus(busId)) return static_cast<DistortionMode>(bus->distortionParams.mode.load(std::memory_order_relaxed));
    return DistortionMode::SoftClip;
}

float Engine::getBusDistortionDrive(int busId) const {
    if (auto* bus = findBus(busId)) return bus->distortionParams.drive.load(std::memory_order_relaxed);
    return 1.0f;
}

float Engine::getBusDistortionMix(int busId) const {
    if (auto* bus = findBus(busId)) return bus->distortionParams.mix.load(std::memory_order_relaxed);
    return 1.0f;
}

float Engine::getBusDistortionOutputGain(int busId) const {
    if (auto* bus = findBus(busId)) return bus->distortionParams.outputGain.load(std::memory_order_relaxed);
    return 1.0f;
}

float Engine::getBusDistortionCrushBits(int busId) const {
    if (auto* bus = findBus(busId)) return bus->distortionParams.crushBits.load(std::memory_order_relaxed);
    return 16.0f;
}

float Engine::getBusDistortionCrushRate(int busId) const {
    if (auto* bus = findBus(busId)) return bus->distortionParams.crushRate.load(std::memory_order_relaxed);
    return 1.0f;
}

bool Engine::getBusEqEnabled(int busId) const {
    if (auto* bus = findBus(busId)) return bus->eqParams.enabled.load(std::memory_order_relaxed);
    return false;
}

float Engine::getBusEqBandGain(int busId, int band) const {
    if (auto* bus = findBus(busId)) {
        if (band >= 0 && band < Equalizer::NUM_BANDS) return bus->eqParams.bandGains[band].load(std::memory_order_relaxed);
    }
    return 0.0f;
}

float Engine::getBusEqMasterGain(int busId) const {
    if (auto* bus = findBus(busId)) return bus->eqParams.masterGain.load(std::memory_order_relaxed);
    return 0.0f;
}

// --- Per-bus compressor sidechain ---

void Engine::setBusCompressorSidechain(int busId, int sidechainBusId)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->compressorParams.sidechainBusId.store(sidechainBusId, std::memory_order_relaxed);
    bus->compressorParams.version.fetch_add(1, std::memory_order_release);
}

// --- Per-bus metering ---

float Engine::getBusPeakL(int busId) const
{
    if (auto* b = findBus(busId)) return b->peakL.load(std::memory_order_relaxed);
    return 0.0f;
}

float Engine::getBusPeakR(int busId) const
{
    if (auto* b = findBus(busId)) return b->peakR.load(std::memory_order_relaxed);
    return 0.0f;
}

float Engine::getBusRmsL(int busId) const
{
    if (auto* b = findBus(busId)) return b->rmsL.load(std::memory_order_relaxed);
    return 0.0f;
}

float Engine::getBusRmsR(int busId) const
{
    if (auto* b = findBus(busId)) return b->rmsR.load(std::memory_order_relaxed);
    return 0.0f;
}

// --- Sample-accurate event scheduling ---

void Engine::scheduleNoteOn(int voiceId, double when)
{
    uint32_t w = eventWrite_.load(std::memory_order_relaxed);
    uint32_t next = (w + 1) % EVENT_RING_SIZE;
    if (next == eventRead_.load(std::memory_order_acquire)) return; // full
    eventRing_[w] = {ScheduledEvent::Type::NoteOn, voiceId, when};
    eventWrite_.store(next, std::memory_order_release);
}

void Engine::scheduleNoteOff(int voiceId, double when)
{
    uint32_t w = eventWrite_.load(std::memory_order_relaxed);
    uint32_t next = (w + 1) % EVENT_RING_SIZE;
    if (next == eventRead_.load(std::memory_order_acquire)) return; // full
    eventRing_[w] = {ScheduledEvent::Type::NoteOff, voiceId, when};
    eventWrite_.store(next, std::memory_order_release);
}

// --- Per-bus effect chain order ---

void Engine::setBusEffectOrder(int busId, const EffectSlot* order, int count)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    int n = std::clamp(count, 0, Bus::NUM_EFFECT_SLOTS);
    bool named[Bus::NUM_EFFECT_SLOTS] = {};
    for (int i = 0; i < n; i++) {
        const int s = static_cast<int>(order[i]);
        bus->effectOrder[i].store(static_cast<uint8_t>(order[i]), std::memory_order_relaxed);
        if (s >= 0 && s < Bus::NUM_EFFECT_SLOTS) named[s] = true;
    }
    // Refill the rest with the slots the list left out, in default order.
    int pos = n;
    for (int s = 0; s < Bus::NUM_EFFECT_SLOTS && pos < Bus::NUM_EFFECT_SLOTS; s++) {
        if (!named[s]) bus->effectOrder[pos++].store(static_cast<uint8_t>(s), std::memory_order_relaxed);
    }
    bus->effectOrderVersion.fetch_add(1, std::memory_order_release);
}

// --- Voice/clip bus routing ---

void Engine::setVoiceBus(int voiceId, int busId)
{
    if (auto* v = findVoice(voiceId))
        v->busId.store(busId, std::memory_order_relaxed);
}

void Engine::setPlaybackBus(int instanceId, int busId)
{
    if (auto* pb = findPlayback(instanceId))
        pb->busId.store(busId, std::memory_order_relaxed);
}

void Engine::setVoiceSend(int voiceId, int sendBusId, float amount)
{
    if (auto* v = findVoice(voiceId)) {
        v->sendBusId.store(sendBusId, std::memory_order_relaxed);
        v->sendAmount.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
    }
}

void Engine::setPlaybackSend(int instanceId, int sendBusId, float amount, float rampSeconds)
{
    if (auto* pb = findPlayback(instanceId)) {
        pb->sendBusId.store(sendBusId, std::memory_order_relaxed);
        pb->sendRamp.set(pb->sendAmount, std::clamp(amount, 0.0f, 1.0f), rampSeconds);
    }
}

void Engine::setBusSend(int busId, int sendBusId, float amount)
{
    if (auto* b = findBus(busId)) {
        b->sendBusId.store(sendBusId, std::memory_order_relaxed);
        b->sendAmount.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
    }
}

} // namespace broaudio
