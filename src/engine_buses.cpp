// Bus management: create/delete, offline effect rendering, gain/pan/solo/mute
// and the per-bus biquad filter slots. Control thread only; the audio-thread
// bus processing is in engine.cpp and engine_bus_fx.cpp.

#include "broaudio/engine.h"
#include "broaudio/dsp/equalizer.h"
#include "engine_internal.h"

#include <cmath>
#include <algorithm>

namespace broaudio {

// ---------------------------------------------------------------------------
// Bus management — RCU for the list, atomics for parameters
// ---------------------------------------------------------------------------

Bus* Engine::findBus(int busId) const
{
    auto currentBuses = buses_.load();
    for (auto& b : *currentBuses) {
        if (b->id == busId) return b.get();
    }
    return nullptr;
}

int Engine::createBus()
{
    std::lock_guard<std::mutex> lock(busWriteMutex_);

    auto bus = std::make_shared<Bus>();
    int id = nextBusId_++;
    bus->id = id;
    bus->parentId.store(MASTER_BUS_ID, std::memory_order_relaxed);
    bus->initAudioState(sampleRate_, MAX_SCRATCH_FRAMES);
    bus->jitPipeline.setDomain(rcu_);
    bus->convolver.setDomain(rcu_);
    syncBusJitTopology(*bus);

    auto newList = std::make_shared<BusList>(*buses_.load());
    newList->push_back(std::move(bus));
    buses_.store(std::move(newList));

    return id;
}

void Engine::deleteBus(int busId)
{
    if (busId == MASTER_BUS_ID) return;  // cannot delete master

    std::lock_guard<std::mutex> lock(busWriteMutex_);

    // Reroute any voices/clips on this bus to master
    auto currentVoices = voices_.load();
    for (auto& v : *currentVoices) {
        if (v->busId.load(std::memory_order_relaxed) == busId)
            v->busId.store(MASTER_BUS_ID, std::memory_order_relaxed);
    }
    auto currentPlaybacks = playbacks_.load();
    for (auto& pb : *currentPlaybacks) {
        if (pb->busId.load(std::memory_order_relaxed) == busId)
            pb->busId.store(MASTER_BUS_ID, std::memory_order_relaxed);
    }

    auto current = buses_.load();
    auto newList = std::make_shared<BusList>();
    newList->reserve(current->size());
    for (auto& b : *current) {
        if (b->id != busId) {
            newList->push_back(b);
        } else if (b->soloed.load(std::memory_order_relaxed)) {
            // Deleting a soloed bus releases its solo so the count can't
            // strand the mixer in solo mode forever.
            soloCount_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    buses_.store(std::move(newList));
}

std::vector<float> Engine::processEffectsOffline(int busId, const float* monoInput, int numSamples)
{
    // Reader scope: findBus and any bus loads inside this routine need
    // their snapshots pinned for the duration.
    RcuDomain::ReadScope rcuScope(rcu_);

    auto* srcBus = findBus(busId);
    if (!srcBus || numSamples <= 0) return {};

    static constexpr int CHUNK = 1024;

    // Create a temporary bus with fresh effect state and cloned params
    Bus temp;
    temp.initAudioState(sampleRate_, CHUNK);

    // Snapshot filter params
    for (int i = 0; i < Bus::MAX_FILTERS; i++) {
        temp.filterParams[i].enabled.store(srcBus->filterParams[i].enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
        temp.filterParams[i].type.store(srcBus->filterParams[i].type.load(std::memory_order_relaxed), std::memory_order_relaxed);
        temp.filterParams[i].frequency.store(srcBus->filterParams[i].frequency.load(std::memory_order_relaxed), std::memory_order_relaxed);
        temp.filterParams[i].Q.store(srcBus->filterParams[i].Q.load(std::memory_order_relaxed), std::memory_order_relaxed);
        temp.filterParams[i].gainDB.store(srcBus->filterParams[i].gainDB.load(std::memory_order_relaxed), std::memory_order_relaxed);
        temp.filterParams[i].version.store(1, std::memory_order_relaxed);
    }

    // Snapshot delay params
    temp.delayParams.enabled.store(srcBus->delayParams.enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.delayParams.time.store(srcBus->delayParams.time.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.delayParams.feedback.store(srcBus->delayParams.feedback.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.delayParams.mix.store(srcBus->delayParams.mix.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.delayParams.version.store(1, std::memory_order_relaxed);

    // Snapshot compressor params
    temp.compressorParams.enabled.store(srcBus->compressorParams.enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.compressorParams.threshold.store(srcBus->compressorParams.threshold.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.compressorParams.ratio.store(srcBus->compressorParams.ratio.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.compressorParams.attackMs.store(srcBus->compressorParams.attackMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.compressorParams.releaseMs.store(srcBus->compressorParams.releaseMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.compressorParams.version.store(1, std::memory_order_relaxed);

    // Snapshot reverb params
    temp.reverbParams.enabled.store(srcBus->reverbParams.enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.reverbParams.roomSize.store(srcBus->reverbParams.roomSize.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.reverbParams.damping.store(srcBus->reverbParams.damping.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.reverbParams.mix.store(srcBus->reverbParams.mix.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.reverbParams.version.store(1, std::memory_order_relaxed);

    // Snapshot chorus params
    temp.chorusParams.enabled.store(srcBus->chorusParams.enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.chorusParams.rate.store(srcBus->chorusParams.rate.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.chorusParams.depth.store(srcBus->chorusParams.depth.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.chorusParams.mix.store(srcBus->chorusParams.mix.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.chorusParams.feedback.store(srcBus->chorusParams.feedback.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.chorusParams.baseDelay.store(srcBus->chorusParams.baseDelay.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.chorusParams.version.store(1, std::memory_order_relaxed);

    // Snapshot distortion params
    temp.distortionParams.enabled.store(srcBus->distortionParams.enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.distortionParams.mode.store(srcBus->distortionParams.mode.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.distortionParams.drive.store(srcBus->distortionParams.drive.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.distortionParams.mix.store(srcBus->distortionParams.mix.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.distortionParams.outputGain.store(srcBus->distortionParams.outputGain.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.distortionParams.crushBits.store(srcBus->distortionParams.crushBits.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.distortionParams.crushRate.store(srcBus->distortionParams.crushRate.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.distortionParams.version.store(1, std::memory_order_relaxed);

    // Snapshot EQ params
    temp.eqParams.enabled.store(srcBus->eqParams.enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    temp.eqParams.masterGain.store(srcBus->eqParams.masterGain.load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int i = 0; i < Equalizer::NUM_BANDS; i++) {
        temp.eqParams.bandGains[i].store(srcBus->eqParams.bandGains[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    temp.eqParams.version.store(1, std::memory_order_relaxed);

    // Clone effect order
    for (int i = 0; i < Bus::NUM_EFFECT_SLOTS; i++) {
        temp.effectOrder[i].store(srcBus->effectOrder[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    temp.effectOrderVersion.store(1, std::memory_order_relaxed);

    // Add extra samples for effect tails (delay, reverb)
    float delaySec = temp.delayParams.time.load(std::memory_order_relaxed);
    float delayFb = temp.delayParams.feedback.load(std::memory_order_relaxed);
    bool hasDelay = temp.delayParams.enabled.load(std::memory_order_relaxed);
    bool hasReverb = temp.reverbParams.enabled.load(std::memory_order_relaxed);
    int tailSamples = 0;
    if (hasDelay) {
        // Approximate delay tail based on feedback decay
        int repeats = delayFb > 0.01f ? static_cast<int>(std::log(0.001f) / std::log(delayFb)) : 0;
        tailSamples = std::max(tailSamples, static_cast<int>(delaySec * sampleRate_) * repeats);
    }
    if (hasReverb) {
        tailSamples = std::max(tailSamples, sampleRate_ * 3); // ~3s reverb tail
    }
    tailSamples = std::min(tailSamples, sampleRate_ * 10); // cap at 10s

    int totalSamples = numSamples + tailSamples;
    std::vector<float> output(totalSamples);

    for (int pos = 0; pos < totalSamples; pos += CHUNK) {
        int frames = std::min(CHUNK, totalSamples - pos);

        // Fill temp bus buffer: mono → stereo (zero-padded for tail)
        for (int i = 0; i < frames; i++) {
            int srcIdx = pos + i;
            float s = (srcIdx < numSamples) ? monoInput[srcIdx] : 0.0f;
            temp.buffer[i * 2]     = s;
            temp.buffer[i * 2 + 1] = s;
        }

        processBusEffects(temp, frames);

        // Mono mixdown to output
        for (int i = 0; i < frames; i++) {
            output[pos + i] = (temp.buffer[i * 2] + temp.buffer[i * 2 + 1]) * 0.5f;
        }
    }

    // Trim trailing silence
    int end = totalSamples - 1;
    while (end > numSamples && std::abs(output[end]) < 0.0001f) end--;
    end = std::max(end, numSamples - 1);
    output.resize(end + 1);

    return output;
}

void Engine::setBusGain(int busId, float gain, float rampSeconds)
{
    if (auto* b = findBus(busId))
        b->gainRamp.set(b->gain, std::clamp(gain, 0.0f, 2.0f), rampSeconds);
}

void Engine::setBusPan(int busId, float pan)
{
    if (auto* b = findBus(busId))
        b->pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void Engine::setBusSolo(int busId, bool solo)
{
    // busWriteMutex_ serialises the flag flip with deleteBus so soloCount_
    // can never leak a count for a bus that was concurrently removed.
    std::lock_guard<std::mutex> lock(busWriteMutex_);
    if (auto* b = findBus(busId)) {
        bool prev = b->soloed.exchange(solo, std::memory_order_relaxed);
        if (prev != solo)
            soloCount_.fetch_add(solo ? 1 : -1, std::memory_order_relaxed);
    }
}

bool Engine::getBusSolo(int busId) const
{
    if (auto* b = findBus(busId))
        return b->soloed.load(std::memory_order_relaxed);
    return false;
}

bool Engine::busAudibleUnderSolo(const BusList& buses, const Bus& bus) const
{
    if (soloCount_.load(std::memory_order_relaxed) <= 0) return true;
    if (bus.soloed.load(std::memory_order_relaxed)) return true;

    // Depth guard: the parent graph is user-assembled; a cycle would
    // otherwise spin the audio thread.
    constexpr int kMaxDepth = 64;

    // Soloed ancestor? (bus lives inside a soloed group — keep it audible)
    int pid = bus.parentId.load(std::memory_order_relaxed);
    for (int guard = 0; pid >= 0 && guard < kMaxDepth; ++guard) {
        const Bus* p = nullptr;
        for (auto& b : buses) {
            if (b->id == pid) { p = b.get(); break; }
        }
        if (!p) break;
        if (p->soloed.load(std::memory_order_relaxed)) return true;
        pid = p->parentId.load(std::memory_order_relaxed);
    }

    // Soloed descendant? (this bus carries a soloed bus's audio to master)
    for (auto& s : buses) {
        if (!s->soloed.load(std::memory_order_relaxed)) continue;
        int q = s->parentId.load(std::memory_order_relaxed);
        for (int guard = 0; q >= 0 && guard < kMaxDepth; ++guard) {
            if (q == bus.id) return true;
            const Bus* p = nullptr;
            for (auto& b : buses) {
                if (b->id == q) { p = b.get(); break; }
            }
            if (!p) break;
            q = p->parentId.load(std::memory_order_relaxed);
        }
    }
    return false;
}

float Engine::getBusGain(int busId) const
{
    if (auto* b = findBus(busId))
        return b->gain.load(std::memory_order_relaxed);
    return 1.0f;
}

float Engine::getBusPan(int busId) const
{
    if (auto* b = findBus(busId))
        return b->pan.load(std::memory_order_relaxed);
    return 0.0f;
}

void Engine::setBusMuted(int busId, bool muted)
{
    if (auto* b = findBus(busId))
        b->muted.store(muted, std::memory_order_relaxed);
}

bool Engine::getBusMuted(int busId) const
{
    if (auto* b = findBus(busId))
        return b->muted.load(std::memory_order_relaxed);
    return false;
}

// --- Per-bus filter control ---

int Engine::allocateBusFilterSlot(int busId)
{
    auto* bus = findBus(busId);
    if (!bus) return -1;
    for (int i = 0; i < Bus::MAX_FILTERS; i++) {
        bool expected = false;
        if (bus->filterParams[i].allocated.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return i;
        }
    }
    return -1;
}

void Engine::releaseBusFilterSlot(int busId, int slot)
{
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return;
    bus->filterParams[slot].enabled.store(false, std::memory_order_relaxed);
    bus->filterParams[slot].version.fetch_add(1, std::memory_order_release);
    bus->filterParams[slot].allocated.store(false, std::memory_order_release);
    syncBusJitTopology(*bus);
}

void Engine::setBusFilterEnabled(int busId, int slot, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return;
    bus->filterParams[slot].enabled.store(enabled, std::memory_order_relaxed);
    bus->filterParams[slot].version.fetch_add(1, std::memory_order_release);
    syncBusJitTopology(*bus);
}

void Engine::setBusFilterType(int busId, int slot, BiquadFilter::Type type)
{
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return;
    bus->filterParams[slot].type.store(static_cast<int>(type), std::memory_order_relaxed);
    bus->filterParams[slot].version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusFilterFrequency(int busId, int slot, float freq)
{
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return;
    bus->filterParams[slot].frequency.store(std::clamp(freq, 20.0f, 20000.0f), std::memory_order_relaxed);
    bus->filterParams[slot].version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusFilterQ(int busId, int slot, float q)
{
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return;
    bus->filterParams[slot].Q.store(std::clamp(q, 0.1f, 30.0f), std::memory_order_relaxed);
    bus->filterParams[slot].version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusFilterGain(int busId, int slot, float gainDB)
{
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return;
    bus->filterParams[slot].gainDB.store(std::clamp(gainDB, -40.0f, 40.0f), std::memory_order_relaxed);
    bus->filterParams[slot].version.fetch_add(1, std::memory_order_release);
}

bool Engine::getBusFilterEnabled(int busId, int slot) const {
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return false;
    return bus->filterParams[slot].enabled.load(std::memory_order_relaxed);
}

BiquadFilter::Type Engine::getBusFilterType(int busId, int slot) const {
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return BiquadFilter::Type::Lowpass;
    return static_cast<BiquadFilter::Type>(bus->filterParams[slot].type.load(std::memory_order_relaxed));
}

float Engine::getBusFilterFrequency(int busId, int slot) const {
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return 1000.0f;
    return bus->filterParams[slot].frequency.load(std::memory_order_relaxed);
}

float Engine::getBusFilterQ(int busId, int slot) const {
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return 1.0f;
    return bus->filterParams[slot].Q.load(std::memory_order_relaxed);
}

float Engine::getBusFilterGain(int busId, int slot) const {
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return 0.0f;
    return bus->filterParams[slot].gainDB.load(std::memory_order_relaxed);
}

} // namespace broaudio
