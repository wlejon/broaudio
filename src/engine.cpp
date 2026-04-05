#include "broaudio/engine.h"
#include "broaudio/synth/oscillator.h"
#include "broaudio/synth/wavetable.h"
#include "broaudio/dsp/equalizer.h"
#include "broaudio/dsp/fft.h"
#include "broaudio/dsp/limiter.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace broaudio {

static constexpr float DEFAULT_ATTACK  = 0.01f;
static constexpr float DEFAULT_DECAY   = 0.1f;
static constexpr float DEFAULT_SUSTAIN = 1.0f;
static constexpr float DEFAULT_RELEASE = 0.04f;
static constexpr float VOICE_AMPLITUDE = 0.1f;
static constexpr int   MAX_SCRATCH_FRAMES = 8192;

// ---------------------------------------------------------------------------
// Engine lifecycle
// ---------------------------------------------------------------------------

Engine::Engine() = default;

Engine::~Engine()
{
    shutdown();
}

bool Engine::init()
{
    if (initialized_) return true;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("broaudio: Failed to init SDL audio: %s", SDL_GetError());
        return false;
    }

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "128");

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = sampleRate_;

    stream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioCallback, this);

    if (!stream_) {
        SDL_Log("broaudio: Failed to open audio device: %s", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(stream_);

    // Create master bus (id 0)
    {
        auto master = std::make_shared<Bus>();
        master->id = MASTER_BUS_ID;
        master->parentId.store(-1, std::memory_order_relaxed);  // no parent
        master->initAudioState(sampleRate_, MAX_SCRATCH_FRAMES);

        auto list = std::make_shared<BusList>();
        list->push_back(std::move(master));
        buses_.store(std::move(list));
    }

    // Pre-allocate scratch buffers
    outputScratch_.resize(MAX_SCRATCH_FRAMES * 2, 0.0f);
    micScratch_.resize(MAX_SCRATCH_FRAMES, 0.0f);

    initialized_ = true;
    SDL_Log("broaudio: initialized %d Hz stereo", sampleRate_);
    return true;
}

void Engine::shutdown()
{
    stopMicCapture();
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    initialized_ = false;
}

double Engine::currentTime() const
{
    return static_cast<double>(samplesGenerated_.load(std::memory_order_relaxed))
           / static_cast<double>(sampleRate_);
}

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
        if (b->id != busId) newList->push_back(b);
    }
    buses_.store(std::move(newList));
}

std::vector<float> Engine::processEffectsOffline(int busId, const float* monoInput, int numSamples)
{
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

void Engine::setBusGain(int busId, float gain)
{
    if (auto* b = findBus(busId))
        b->gain.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
}

void Engine::setBusPan(int busId, float pan)
{
    if (auto* b = findBus(busId))
        b->pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void Engine::setBusMuted(int busId, bool muted)
{
    if (auto* b = findBus(busId))
        b->muted.store(muted, std::memory_order_relaxed);
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
}

void Engine::setBusFilterEnabled(int busId, int slot, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus || slot < 0 || slot >= Bus::MAX_FILTERS) return;
    bus->filterParams[slot].enabled.store(enabled, std::memory_order_relaxed);
    bus->filterParams[slot].version.fetch_add(1, std::memory_order_release);
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
    int n = std::min(count, Bus::NUM_EFFECT_SLOTS);
    for (int i = 0; i < n; i++)
        bus->effectOrder[i].store(static_cast<uint8_t>(order[i]), std::memory_order_relaxed);
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

void Engine::setPlaybackSend(int instanceId, int sendBusId, float amount)
{
    if (auto* pb = findPlayback(instanceId)) {
        pb->sendBusId.store(sendBusId, std::memory_order_relaxed);
        pb->sendAmount.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
    }
}

void Engine::setBusSend(int busId, int sendBusId, float amount)
{
    if (auto* b = findBus(busId)) {
        b->sendBusId.store(sendBusId, std::memory_order_relaxed);
        b->sendAmount.store(std::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
    }
}

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
    float sr = static_cast<float>(sampleRate_);
    voice->attackRate.store(1.0f / (DEFAULT_ATTACK * sr), std::memory_order_relaxed);
    voice->decayCoeff.store(std::exp(-3.0f / (DEFAULT_DECAY * sr)), std::memory_order_relaxed);
    voice->sustainLevel.store(DEFAULT_SUSTAIN, std::memory_order_relaxed);
    voice->releaseCoeff.store(std::exp(-3.0f / (DEFAULT_RELEASE * sr)), std::memory_order_relaxed);

    auto newList = std::make_shared<VoiceList>(*voices_.load());
    newList->push_back(voice);
    voices_.store(std::move(newList));

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
    if (auto* v = findVoice(id)) {
        v->startTime.store(when, std::memory_order_relaxed);
        v->triggerStart.store(true, std::memory_order_release);
    }
}

void Engine::stopVoice(int id, double /*when*/)
{
    if (auto* v = findVoice(id)) {
        v->triggerRelease.store(true, std::memory_order_release);
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

// --- Spatial sources (voice) ---

static void setSpatialEnabled(SpatialSource& s, bool enabled) { s.spatialEnabled.store(enabled, std::memory_order_relaxed); }
static void setSpatialPosition(SpatialSource& s, float x, float y, float z) {
    s.posX.store(x, std::memory_order_relaxed);
    s.posY.store(y, std::memory_order_relaxed);
    s.posZ.store(z, std::memory_order_relaxed);
}
static void setSpatialRefDistance(SpatialSource& s, float d) { s.refDistance.store(std::max(0.001f, d), std::memory_order_relaxed); }
static void setSpatialMaxDistance(SpatialSource& s, float d) { s.maxDistance.store(std::max(0.001f, d), std::memory_order_relaxed); }
static void setSpatialRolloff(SpatialSource& s, float r) { s.rolloff.store(std::max(0.0f, r), std::memory_order_relaxed); }
static void setSpatialDistanceModel(SpatialSource& s, DistanceModel m) { s.distanceModel.store(static_cast<int>(m), std::memory_order_relaxed); }

void Engine::setVoiceSpatialEnabled(int id, bool enabled) { if (auto* v = findVoice(id)) setSpatialEnabled(v->spatial, enabled); }
void Engine::setVoiceSpatialPosition(int id, float x, float y, float z) { if (auto* v = findVoice(id)) setSpatialPosition(v->spatial, x, y, z); }
void Engine::setVoiceSpatialRefDistance(int id, float d) { if (auto* v = findVoice(id)) setSpatialRefDistance(v->spatial, d); }
void Engine::setVoiceSpatialMaxDistance(int id, float d) { if (auto* v = findVoice(id)) setSpatialMaxDistance(v->spatial, d); }
void Engine::setVoiceSpatialRolloff(int id, float r) { if (auto* v = findVoice(id)) setSpatialRolloff(v->spatial, r); }
void Engine::setVoiceSpatialDistanceModel(int id, DistanceModel m) { if (auto* v = findVoice(id)) setSpatialDistanceModel(v->spatial, m); }

void Engine::setPlaybackSpatialEnabled(int id, bool enabled) { if (auto* pb = findPlayback(id)) setSpatialEnabled(pb->spatial, enabled); }
void Engine::setPlaybackSpatialPosition(int id, float x, float y, float z) { if (auto* pb = findPlayback(id)) setSpatialPosition(pb->spatial, x, y, z); }
void Engine::setPlaybackSpatialRefDistance(int id, float d) { if (auto* pb = findPlayback(id)) setSpatialRefDistance(pb->spatial, d); }
void Engine::setPlaybackSpatialMaxDistance(int id, float d) { if (auto* pb = findPlayback(id)) setSpatialMaxDistance(pb->spatial, d); }
void Engine::setPlaybackSpatialRolloff(int id, float r) { if (auto* pb = findPlayback(id)) setSpatialRolloff(pb->spatial, r); }
void Engine::setPlaybackSpatialDistanceModel(int id, DistanceModel m) { if (auto* pb = findPlayback(id)) setSpatialDistanceModel(pb->spatial, m); }

// ---------------------------------------------------------------------------
// Microphone capture
// ---------------------------------------------------------------------------

bool Engine::startMicCapture()
{
    if (micCapturing_) return true;
    if (!initialized_) return false;

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "128");

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = sampleRate_;

    micStream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, micCallback, this);

    if (!micStream_) {
        SDL_Log("broaudio: Failed to open mic device: %s", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(micStream_);
    micCapturing_ = true;
    return true;
}

void Engine::stopMicCapture()
{
    if (!micCapturing_) return;
    if (micStream_) {
        SDL_DestroyAudioStream(micStream_);
        micStream_ = nullptr;
    }
    micCapturing_ = false;
}

void Engine::micCallback(void* userdata, SDL_AudioStream* stream,
                          int additional_amount, int /*total_amount*/)
{
    auto* engine = static_cast<Engine*>(userdata);

    int avail = SDL_GetAudioStreamAvailable(stream);
    if (avail <= 0) return;

    int numSamples = avail / static_cast<int>(sizeof(float));
    if (static_cast<size_t>(numSamples) > engine->micScratch_.size())
        engine->micScratch_.resize(numSamples);
    float* buffer = engine->micScratch_.data();

    int got = SDL_GetAudioStreamData(stream, buffer, avail);
    if (got > 0) {
        int samplesGot = got / static_cast<int>(sizeof(float));
        {
            std::lock_guard<std::mutex> lock(engine->micMutex_);
            engine->micBuffer_.write(buffer, samplesGot);
        }
        int cap = MIC_FIFO_SIZE;
        uint64_t wp = engine->micPlaybackWritePos_.load(std::memory_order_relaxed);
        for (int i = 0; i < samplesGot; i++) {
            engine->micPlayback_[static_cast<int>((wp + i) % cap)] = buffer[i];
        }
        engine->micPlaybackWritePos_.store(wp + samplesGot, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void Engine::startRecording()
{
    recordStartPos_ = recordWritePos_.load(std::memory_order_relaxed);
    recording_.store(true, std::memory_order_release);
}

void Engine::stopRecording()
{
    recording_.store(false, std::memory_order_release);

    uint64_t endPos = recordWritePos_.load(std::memory_order_acquire);
    uint64_t startPos = recordStartPos_;
    int count = static_cast<int>(endPos - startPos);
    if (count <= 0) {
        recordOutput_.clear();
        return;
    }
    if (count > RECORD_RING_SIZE) {
        startPos = endPos - RECORD_RING_SIZE;
        count = RECORD_RING_SIZE;
    }
    recordOutput_.resize(count);
    for (int i = 0; i < count; i++) {
        recordOutput_[i] = recordRing_[static_cast<int>((startPos + i) % RECORD_RING_SIZE)];
    }
}

// ---------------------------------------------------------------------------
// Spectrum analysis
// ---------------------------------------------------------------------------

int Engine::getSpectrum(float* outMagnitudes, int numBins) const
{
    if (numBins <= 0 || numBins > 8192) return 0;

    // Ensure power of 2
    int n = 1;
    while (n < numBins) n <<= 1;
    if (n > 8192) n = 8192;

    // Read recent samples from the analysis buffer
    std::vector<float> real(n, 0.0f);
    std::vector<float> imag(n, 0.0f);

    // Read latest samples from the analysis ring buffer (benign tearing acceptable for viz)
    outputBuffer_.readLatest(real.data(), n);

    // Apply Hann window
    for (int i = 0; i < n; i++) {
        float w = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(n)));
        real[i] *= w;
    }

    fft(real.data(), imag.data(), n);

    // Compute magnitudes (only first half — Nyquist)
    int outBins = n / 2;
    if (outBins > numBins) outBins = numBins;
    float invN = 1.0f / static_cast<float>(n);
    for (int i = 0; i < outBins; i++) {
        float re = real[i] * invN;
        float im = imag[i] * invN;
        outMagnitudes[i] = std::sqrt(re * re + im * im) * 2.0f;
    }

    return outBins;
}

// ---------------------------------------------------------------------------
// Audio Clips — RCU for clip and playback lists
// ---------------------------------------------------------------------------

AudioClip* Engine::findClip(int clipId) const
{
    auto currentClips = clips_.load();
    for (auto& c : *currentClips) {
        if (c->id == clipId) return c.get();
    }
    return nullptr;
}

ClipPlayback* Engine::findPlayback(int instanceId) const
{
    auto currentPlaybacks = playbacks_.load();
    for (auto& pb : *currentPlaybacks) {
        if (pb->id == instanceId && pb->active.load(std::memory_order_relaxed))
            return pb.get();
    }
    return nullptr;
}

int Engine::createClip(const float* samples, int numSamples, int channels)
{
    if (numSamples <= 0 || !samples || channels < 1 || channels > 2) return -1;

    auto clip = std::make_shared<AudioClip>();
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    clip->id = nextClipId_++;
    clip->channels = channels;
    clip->samples.assign(samples, samples + numSamples);

    int id = clip->id;
    auto newList = std::make_shared<ClipList>(*clips_.load());
    newList->push_back(std::move(clip));
    clips_.store(std::move(newList));

    return id;
}

void Engine::deleteClip(int clipId)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);

    auto currentPB = playbacks_.load();
    auto newPB = std::make_shared<PlaybackList>();
    for (auto& pb : *currentPB) {
        if (pb->clipId == clipId) {
            pb->playing.store(false, std::memory_order_relaxed);
            pb->active.store(false, std::memory_order_relaxed);
        } else {
            newPB->push_back(pb);
        }
    }
    playbacks_.store(std::move(newPB));

    auto currentClips = clips_.load();
    auto newClips = std::make_shared<ClipList>();
    for (auto& c : *currentClips) {
        if (c->id != clipId) newClips->push_back(c);
    }
    clips_.store(std::move(newClips));
}

int Engine::getClipSampleCount(int clipId) const
{
    if (auto* c = findClip(clipId)) return c->numFrames();
    return 0;
}

int Engine::getClipChannels(int clipId) const
{
    if (auto* c = findClip(clipId)) return c->channels;
    return 0;
}

void Engine::getClipWaveform(int clipId, float* outMinMax, int numBins) const
{
    auto* clip = findClip(clipId);
    if (!clip || clip->samples.empty()) {
        for (int i = 0; i < numBins * 2; i++) outMinMax[i] = 0.0f;
        return;
    }

    int totalFrames = clip->numFrames();
    int ch = clip->channels;
    float framesPerBin = static_cast<float>(totalFrames) / static_cast<float>(numBins);

    for (int b = 0; b < numBins; b++) {
        int startFrame = static_cast<int>(b * framesPerBin);
        int endFrame = static_cast<int>((b + 1) * framesPerBin);
        endFrame = std::min(endFrame, totalFrames);

        float minVal = 1.0f, maxVal = -1.0f;
        for (int f = startFrame; f < endFrame; f++) {
            float s;
            if (ch == 2) {
                s = (clip->samples[f * 2] + clip->samples[f * 2 + 1]) * 0.5f;
            } else {
                s = clip->samples[f];
            }
            if (s < minVal) minVal = s;
            if (s > maxVal) maxVal = s;
        }
        outMinMax[b * 2] = minVal;
        outMinMax[b * 2 + 1] = maxVal;
    }
}

// ---------------------------------------------------------------------------
// Clip Playback
// ---------------------------------------------------------------------------

int Engine::playClip(int clipId, float gain, bool loop)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    if (!findClip(clipId)) return -1;

    auto pb = std::make_shared<ClipPlayback>();
    pb->id = nextPlaybackId_++;
    pb->clipId = clipId;
    pb->gain.store(gain, std::memory_order_relaxed);
    pb->looping.store(loop, std::memory_order_relaxed);
    pb->playing.store(true, std::memory_order_relaxed);
    pb->active.store(true, std::memory_order_relaxed);
    pb->playPos.store(0, std::memory_order_relaxed);
    pb->regionStart.store(0, std::memory_order_relaxed);
    pb->regionEnd.store(0, std::memory_order_relaxed);

    int id = pb->id;
    auto newList = std::make_shared<PlaybackList>(*playbacks_.load());
    newList->push_back(std::move(pb));
    playbacks_.store(std::move(newList));
    return id;
}

void Engine::stopPlayback(int instanceId)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    auto current = playbacks_.load();
    auto newList = std::make_shared<PlaybackList>();
    for (auto& pb : *current) {
        if (pb->id == instanceId) {
            pb->playing.store(false, std::memory_order_relaxed);
            pb->active.store(false, std::memory_order_relaxed);
        } else {
            newList->push_back(pb);
        }
    }
    playbacks_.store(std::move(newList));
}

void Engine::setPlaybackGain(int instanceId, float gain)
{
    if (auto* pb = findPlayback(instanceId))
        pb->gain.store(gain, std::memory_order_relaxed);
}

void Engine::setPlaybackLoop(int instanceId, bool loop)
{
    if (auto* pb = findPlayback(instanceId))
        pb->looping.store(loop, std::memory_order_relaxed);
}

void Engine::setPlaybackPlaying(int instanceId, bool playing)
{
    if (auto* pb = findPlayback(instanceId)) {
        if (playing && !pb->playing.load(std::memory_order_relaxed))
            pb->playPos.store(0, std::memory_order_relaxed);
        pb->playing.store(playing, std::memory_order_relaxed);
    }
}

void Engine::setPlaybackRate(int instanceId, float rate)
{
    if (auto* pb = findPlayback(instanceId))
        pb->rate.store(std::clamp(rate, 0.01f, 16.0f), std::memory_order_relaxed);
}

void Engine::setPlaybackPan(int instanceId, float pan)
{
    if (auto* pb = findPlayback(instanceId))
        pb->pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void Engine::setPlaybackRegion(int instanceId, int start, int end)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    if (auto* pb = findPlayback(instanceId)) {
        auto* clip = findClip(pb->clipId);
        if (!clip) return;
        int maxLen = clip->numFrames();
        int rs = std::clamp(start, 0, maxLen);
        int re = std::clamp(end, rs, maxLen);
        pb->regionStart.store(rs, std::memory_order_relaxed);
        pb->regionEnd.store(re, std::memory_order_relaxed);
        pb->playPos.store(0, std::memory_order_relaxed);
    }
}

float Engine::getPlaybackPosition(int instanceId) const
{
    auto* pb = findPlayback(instanceId);
    if (!pb) return 0.0f;
    auto* clip = findClip(pb->clipId);
    if (!clip) return 0.0f;

    int re = pb->regionEnd.load(std::memory_order_relaxed);
    int rs = pb->regionStart.load(std::memory_order_relaxed);
    int end = re > 0 ? re : clip->numFrames();
    int len = end - rs;
    if (len <= 0) return 0.0f;
    uint64_t pos = pb->playPos.load(std::memory_order_relaxed);
    int intPos = static_cast<int>(pos >> 16);
    return static_cast<float>(intPos % len) / static_cast<float>(len);
}

// ---------------------------------------------------------------------------
// Bus effect processing — audio thread only
// ---------------------------------------------------------------------------

void Engine::processBusFilters(Bus& bus, float* buf, int numFrames)
{
    for (int f = 0; f < Bus::MAX_FILTERS; f++) {
        uint32_t ver = bus.filterParams[f].version.load(std::memory_order_acquire);
        if (ver != bus.filterVersions[f]) {
            bus.filterVersions[f] = ver;
            bool enabled = bus.filterParams[f].enabled.load(std::memory_order_relaxed);
            bus.filters[f].enabled = enabled;
            if (enabled) {
                bus.filters[f].type = static_cast<BiquadFilter::Type>(
                    bus.filterParams[f].type.load(std::memory_order_relaxed));
                bus.filters[f].frequency = bus.filterParams[f].frequency.load(std::memory_order_relaxed);
                bus.filters[f].Q = bus.filterParams[f].Q.load(std::memory_order_relaxed);
                bus.filters[f].gainDB = bus.filterParams[f].gainDB.load(std::memory_order_relaxed);
                bus.filters[f].computeCoefficients(sampleRate_);
            } else {
                bus.filters[f].reset();
            }
        }
        if (!bus.filters[f].enabled) continue;
        for (int i = 0; i < numFrames; i++) {
            buf[i * 2]     = bus.filters[f].process(buf[i * 2], 0);
            buf[i * 2 + 1] = bus.filters[f].process(buf[i * 2 + 1], 1);
        }
    }
}

void Engine::processBusDelay(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.delayParams.version.load(std::memory_order_acquire);
    if (ver != bus.delayVersion) {
        bus.delayVersion = ver;
        bus.delay.enabled = bus.delayParams.enabled.load(std::memory_order_relaxed);
        float delaySec = bus.delayParams.time.load(std::memory_order_relaxed);
        int maxSamples = static_cast<int>(bus.delay.buffer.size());
        bus.delay.delaySamples = std::clamp(
            static_cast<int>(delaySec * sampleRate_), 1, maxSamples - 1);
        bus.delay.feedback = bus.delayParams.feedback.load(std::memory_order_relaxed);
        bus.delay.mix = bus.delayParams.mix.load(std::memory_order_relaxed);
    }
    if (bus.delay.enabled) {
        bus.delay.processStereo(buf, numFrames);
    }
}

void Engine::processBusCompressor(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.compressorParams.version.load(std::memory_order_acquire);
    if (ver != bus.compressorVersion) {
        bus.compressorVersion = ver;
        bus.compressor.threshold = bus.compressorParams.threshold.load(std::memory_order_relaxed);
        bus.compressor.ratio = bus.compressorParams.ratio.load(std::memory_order_relaxed);
        float attackMs = bus.compressorParams.attackMs.load(std::memory_order_relaxed);
        float releaseMs = bus.compressorParams.releaseMs.load(std::memory_order_relaxed);
        bus.compressor.attackCoeff = 1.0f - std::exp(-1.0f / (attackMs * 0.001f * static_cast<float>(sampleRate_)));
        bus.compressor.releaseCoeff = 1.0f - std::exp(-1.0f / (releaseMs * 0.001f * static_cast<float>(sampleRate_)));
    }
    if (bus.compressorParams.enabled.load(std::memory_order_relaxed)) {
        int scBusId = bus.compressorParams.sidechainBusId.load(std::memory_order_relaxed);
        if (scBusId >= 0) {
            // Sidechain: detect level from another bus's buffer
            auto currentBuses = buses_.load();
            for (auto& scBus : *currentBuses) {
                if (scBus->id == scBusId) {
                    bus.compressor.processStereoWithSidechain(buf, scBus->buffer.data(), numFrames);
                    return;
                }
            }
        }
        bus.compressor.processStereo(buf, numFrames);
    }
}

void Engine::processBusChorus(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.chorusParams.version.load(std::memory_order_acquire);
    if (ver != bus.chorusVersion) {
        bus.chorusVersion = ver;
        bus.chorus.enabled = bus.chorusParams.enabled.load(std::memory_order_relaxed);
        bus.chorus.rate = bus.chorusParams.rate.load(std::memory_order_relaxed);
        bus.chorus.depth = bus.chorusParams.depth.load(std::memory_order_relaxed);
        bus.chorus.mix = bus.chorusParams.mix.load(std::memory_order_relaxed);
        bus.chorus.feedback = bus.chorusParams.feedback.load(std::memory_order_relaxed);
        bus.chorus.baseDelay = bus.chorusParams.baseDelay.load(std::memory_order_relaxed);
    }
    if (bus.chorus.enabled) {
        bus.chorus.processStereo(buf, numFrames);
    }
}

void Engine::processBusReverb(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.reverbParams.version.load(std::memory_order_acquire);
    if (ver != bus.reverbVersion) {
        bus.reverbVersion = ver;
        bus.reverb.enabled = bus.reverbParams.enabled.load(std::memory_order_relaxed);
        bus.reverb.roomSize = bus.reverbParams.roomSize.load(std::memory_order_relaxed);
        bus.reverb.damping = bus.reverbParams.damping.load(std::memory_order_relaxed);
        bus.reverb.mix = bus.reverbParams.mix.load(std::memory_order_relaxed);
    }
    if (bus.reverb.enabled) {
        bus.reverb.processStereo(buf, numFrames);
    }
}

void Engine::processBusEqualizer(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.eqParams.version.load(std::memory_order_acquire);
    if (ver != bus.eqVersion) {
        bus.eqVersion = ver;
        bool enabled = bus.eqParams.enabled.load(std::memory_order_relaxed);
        bus.equalizer.setEnabled(enabled);
        if (enabled) {
            bus.equalizer.setSampleRate(sampleRate_);
            bus.equalizer.setMasterGain(bus.eqParams.masterGain.load(std::memory_order_relaxed));
            for (int b = 0; b < Equalizer::NUM_BANDS; b++) {
                bus.equalizer.setBandGain(b, bus.eqParams.bandGains[b].load(std::memory_order_relaxed));
            }
        }
    }
    if (bus.equalizer.isEnabled()) {
        bus.equalizer.processStereoInterleaved(buf, numFrames);
    }
}

void Engine::updateBusMeters(Bus& bus, int numFrames)
{
    float* buf = bus.buffer.data();
    float pL = 0.0f, pR = 0.0f;
    float sumSqL = 0.0f, sumSqR = 0.0f;
    for (int i = 0; i < numFrames; i++) {
        float l = std::fabs(buf[i * 2]);
        float r = std::fabs(buf[i * 2 + 1]);
        if (l > pL) pL = l;
        if (r > pR) pR = r;
        sumSqL += buf[i * 2] * buf[i * 2];
        sumSqR += buf[i * 2 + 1] * buf[i * 2 + 1];
    }
    bus.peakL.store(pL, std::memory_order_relaxed);
    bus.peakR.store(pR, std::memory_order_relaxed);
    float invN = 1.0f / static_cast<float>(std::max(numFrames, 1));
    bus.rmsL.store(std::sqrt(sumSqL * invN), std::memory_order_relaxed);
    bus.rmsR.store(std::sqrt(sumSqR * invN), std::memory_order_relaxed);
}

void Engine::processBusEffects(Bus& bus, int numFrames)
{
    float* buf = bus.buffer.data();

    uint32_t ver = bus.effectOrderVersion.load(std::memory_order_acquire);
    if (ver != bus.effectOrderVersionSeen) {
        bus.effectOrderVersionSeen = ver;
        for (int i = 0; i < Bus::NUM_EFFECT_SLOTS; i++)
            bus.effectOrderCache[i] = bus.effectOrder[i].load(std::memory_order_relaxed);
    }

    for (int i = 0; i < Bus::NUM_EFFECT_SLOTS; i++) {
        switch (static_cast<EffectSlot>(bus.effectOrderCache[i])) {
            case EffectSlot::Filter:     processBusFilters(bus, buf, numFrames); break;
            case EffectSlot::Delay:      processBusDelay(bus, buf, numFrames); break;
            case EffectSlot::Compressor: processBusCompressor(bus, buf, numFrames); break;
            case EffectSlot::Chorus:     processBusChorus(bus, buf, numFrames); break;
            case EffectSlot::Reverb:     processBusReverb(bus, buf, numFrames); break;
            case EffectSlot::Equalizer:  processBusEqualizer(bus, buf, numFrames); break;
            default: break;
        }
    }

    // Per-bus soft clipper: safety net so no effect combination can produce
    // samples that clip the output. Transparent below ±0.8, smoothly
    // saturates above. Runs after all effects, before metering.
    for (int i = 0; i < numFrames * 2; i++) {
        buf[i] = softLimit(buf[i]);
    }

    updateBusMeters(bus, numFrames);
}

void Engine::mixBusIntoParent(Bus& child, Bus& parent, int numFrames)
{
    if (child.muted.load(std::memory_order_relaxed)) return;

    float g = child.gain.load(std::memory_order_relaxed);
    float panVal = child.pan.load(std::memory_order_relaxed);
    float panL, panR;
    panGains(panVal, panL, panR);

    float* src = child.buffer.data();
    float* dst = parent.buffer.data();

    for (int i = 0; i < numFrames; i++) {
        float L = src[i * 2]     * g;
        float R = src[i * 2 + 1] * g;
        // Apply bus panning (cross-mix stereo signal)
        dst[i * 2]     += L * panL + R * (1.0f - panR);
        dst[i * 2 + 1] += R * panR + L * (1.0f - panL);
    }
}

// ---------------------------------------------------------------------------
// Output audio callback + synthesis — LOCK-FREE
// ---------------------------------------------------------------------------

void Engine::audioCallback(void* userdata, SDL_AudioStream* stream,
                            int additional_amount, int /*total_amount*/)
{
    auto* engine = static_cast<Engine*>(userdata);
    int numFloats = additional_amount / static_cast<int>(sizeof(float));
    int numFrames = numFloats / 2;
    if (numFrames <= 0) return;

    if (static_cast<size_t>(numFloats) > engine->outputScratch_.size())
        engine->outputScratch_.resize(numFloats);
    float* buffer = engine->outputScratch_.data();

    // Load bus list (lock-free RCU read)
    auto currentBuses = engine->buses_.load();

    // Clear all bus buffers
    for (auto& bus : *currentBuses) {
        int frames = std::min(numFrames, static_cast<int>(bus->buffer.size()) / 2);
        bus->clearBuffer(frames);
    }

    // Generate voices into their target bus buffers
    engine->generateSamples(numFrames, *currentBuses);

    // Mix clip playback into target bus buffers
    {
        auto currentClips = engine->clips_.load();
        auto currentPlaybacks = engine->playbacks_.load();
        for (auto& pb : *currentPlaybacks) {
            if (!pb->active.load(std::memory_order_relaxed)) continue;
            if (!pb->playing.load(std::memory_order_relaxed)) continue;

            AudioClip* clip = nullptr;
            for (auto& c : *currentClips) {
                if (c->id == pb->clipId) { clip = c.get(); break; }
            }
            if (!clip) continue;

            // Find target bus buffer
            int targetBusId = pb->busId.load(std::memory_order_relaxed);
            float* targetBuf = nullptr;
            for (auto& bus : *currentBuses) {
                if (bus->id == targetBusId) { targetBuf = bus->buffer.data(); break; }
            }
            if (!targetBuf) {
                // Fallback to master
                for (auto& bus : *currentBuses) {
                    if (bus->id == MASTER_BUS_ID) { targetBuf = bus->buffer.data(); break; }
                }
            }
            if (!targetBuf) continue;

            int start = pb->regionStart.load(std::memory_order_relaxed);
            int end = pb->regionEnd.load(std::memory_order_relaxed);
            end = end > 0 ? end : clip->numFrames();
            int len = end - start;
            if (len <= 0) continue;

            uint64_t pos = pb->playPos.load(std::memory_order_relaxed);
            float g = pb->gain.load(std::memory_order_relaxed);
            float rate = pb->rate.load(std::memory_order_relaxed);
            bool looping = pb->looping.load(std::memory_order_relaxed);
            float clipPan = pb->pan.load(std::memory_order_relaxed);
            int ch = clip->channels;

            // Spatial override for clip playback
            if (pb->spatial.spatialEnabled.load(std::memory_order_relaxed)) {
                float spatialPan = 0.0f;
                float spatialGain = computeSpatial(engine->listener_, pb->spatial, spatialPan);
                g *= spatialGain;
                clipPan = spatialPan;
            }

            float panL, panR;
            panGains(clipPan, panL, panR);

            // Find clip send bus buffer (if configured)
            int clipSendId = pb->sendBusId.load(std::memory_order_relaxed);
            float clipSendAmt = pb->sendAmount.load(std::memory_order_relaxed);
            float* clipSendBuf = nullptr;
            if (clipSendId >= 0 && clipSendAmt > 0.0f) {
                for (auto& bus : *currentBuses) {
                    if (bus->id == clipSendId) { clipSendBuf = bus->buffer.data(); break; }
                }
            }

            constexpr int FRAC_BITS = 16;
            constexpr uint64_t FRAC_MASK = (1ULL << FRAC_BITS) - 1;
            uint64_t increment = static_cast<uint64_t>(rate * (1 << FRAC_BITS) + 0.5f);

            for (int i = 0; i < numFrames; i++) {
                int intPos = static_cast<int>(pos >> FRAC_BITS);
                if (looping) {
                    intPos = intPos % len;
                } else if (intPos >= len) {
                    pb->playing.store(false, std::memory_order_relaxed);
                    pb->active.store(false, std::memory_order_relaxed);
                    break;
                }
                float frac = static_cast<float>(pos & FRAC_MASK) / (1 << FRAC_BITS);
                int nextIdx = intPos + 1;
                if (nextIdx >= len) nextIdx = looping ? 0 : intPos;

                float outL, outR;
                if (ch == 2) {
                    // Stereo clip: interleaved L/R pairs
                    int idx0 = (start + intPos) * 2;
                    int idx1 = (start + nextIdx) * 2;
                    float L0 = clip->samples[idx0];
                    float R0 = clip->samples[idx0 + 1];
                    float L1 = clip->samples[idx1];
                    float R1 = clip->samples[idx1 + 1];
                    float sL = (L0 + frac * (L1 - L0)) * g;
                    float sR = (R0 + frac * (R1 - R0)) * g;
                    // Pan acts as balance: panL/panR crossfade the stereo image
                    outL = sL * panL + sR * (1.0f - panR);
                    outR = sR * panR + sL * (1.0f - panL);
                } else {
                    // Mono clip
                    float s0 = clip->samples[start + intPos];
                    float s1 = clip->samples[start + nextIdx];
                    float sample = (s0 + frac * (s1 - s0)) * g;
                    outL = sample * panL;
                    outR = sample * panR;
                }

                targetBuf[i * 2]     += outL;
                targetBuf[i * 2 + 1] += outR;

                // Clip aux send
                if (clipSendBuf) {
                    clipSendBuf[i * 2]     += outL * clipSendAmt;
                    clipSendBuf[i * 2 + 1] += outR * clipSendAmt;
                }

                pos += increment;
            }
            pb->playPos.store(pos, std::memory_order_relaxed);
        }
    }

    // Mix mic into target bus (if routed)
    int micBusId = engine->micBusId_.load(std::memory_order_relaxed);
    if (micBusId >= 0 && !engine->micMuted_.load(std::memory_order_relaxed)) {
        float micGain = engine->micMonitorGain_.load(std::memory_order_relaxed);
        uint64_t wp = engine->micPlaybackWritePos_.load(std::memory_order_acquire);
        uint64_t rp = engine->micPlaybackReadPos_;
        int cap = Engine::MIC_FIFO_SIZE;
        uint64_t available = wp - rp;

        int targetLatency = numFrames;
        if (available > static_cast<uint64_t>(cap - numFrames)) {
            rp = wp - targetLatency;
            available = targetLatency;
        }

        int toRead = static_cast<int>(std::min(available, static_cast<uint64_t>(numFrames)));

        // Find target bus
        Bus* micBus = nullptr;
        for (auto& bus : *currentBuses) {
            if (bus->id == micBusId) { micBus = bus.get(); break; }
        }
        if (micBus) {
            for (int i = 0; i < toRead; i++) {
                int idx = static_cast<int>((rp + i) % cap);
                float s = engine->micPlayback_[idx] * micGain;
                micBus->buffer[i * 2]     += s;
                micBus->buffer[i * 2 + 1] += s;
            }
        }
        engine->micPlaybackReadPos_ = rp + toRead;
    }

    // Record tap (mono mixdown from master bus, before effects)
    Bus* masterBus = nullptr;
    for (auto& bus : *currentBuses) {
        if (bus->id == MASTER_BUS_ID) { masterBus = bus.get(); break; }
    }

    if (engine->recording_.load(std::memory_order_relaxed) && masterBus) {
        uint64_t wp = engine->recordWritePos_.load(std::memory_order_relaxed);
        for (int i = 0; i < numFrames; i++) {
            float mono = (masterBus->buffer[i * 2] + masterBus->buffer[i * 2 + 1]) * 0.5f;
            engine->recordRing_[static_cast<int>((wp + i) % RECORD_RING_SIZE)] = mono;
        }
        engine->recordWritePos_.store(wp + numFrames, std::memory_order_release);
    }

    // Process child buses: apply effects, then mix into parent + send
    for (auto& bus : *currentBuses) {
        if (bus->id == MASTER_BUS_ID) continue;  // master processed last

        engine->processBusEffects(*bus, numFrames);

        // Find parent and mix into it
        int parentId = bus->parentId.load(std::memory_order_relaxed);
        for (auto& parent : *currentBuses) {
            if (parent->id == parentId) {
                engine->mixBusIntoParent(*bus, *parent, numFrames);
                break;
            }
        }

        // Bus-to-bus aux send (post-effects, post-fader)
        int busSendId = bus->sendBusId.load(std::memory_order_relaxed);
        float busSendAmt = bus->sendAmount.load(std::memory_order_relaxed);
        if (busSendId >= 0 && busSendAmt > 0.0f) {
            for (auto& sendTarget : *currentBuses) {
                if (sendTarget->id == busSendId) {
                    float busGain = bus->gain.load(std::memory_order_relaxed) * busSendAmt;
                    float* src = bus->buffer.data();
                    float* dst = sendTarget->buffer.data();
                    for (int i = 0; i < numFrames * 2; i++) {
                        dst[i] += src[i] * busGain;
                    }
                    break;
                }
            }
        }
    }

    // Process master bus effects
    if (masterBus) {
        engine->processBusEffects(*masterBus, numFrames);
    }

    // Copy master bus to output buffer, apply master gain + limiter
    if (masterBus) {
        float mg = engine->masterGain_.load(std::memory_order_relaxed);
        for (int i = 0; i < numFloats; i++) {
            buffer[i] = masterBus->buffer[i] * mg;
        }
        engine->masterLimiter_.process(buffer, static_cast<size_t>(numFrames));
    } else {
        std::memset(buffer, 0, numFloats * sizeof(float));
    }

    // Mix mic monitor (direct-to-output, only when not routed through a bus)
    if (micBusId < 0 && !engine->micMuted_.load(std::memory_order_relaxed)) {
        float micGain = engine->micMonitorGain_.load(std::memory_order_relaxed);
        uint64_t wp = engine->micPlaybackWritePos_.load(std::memory_order_acquire);
        uint64_t rp = engine->micPlaybackReadPos_;
        int cap = Engine::MIC_FIFO_SIZE;
        uint64_t available = wp - rp;

        int targetLatency = numFrames;
        if (available > static_cast<uint64_t>(cap - numFrames)) {
            rp = wp - targetLatency;
            available = targetLatency;
        }

        int toRead = static_cast<int>(std::min(available, static_cast<uint64_t>(numFrames)));
        for (int i = 0; i < toRead; i++) {
            int idx = static_cast<int>((rp + i) % cap);
            float s = engine->micPlayback_[idx] * micGain;
            buffer[i * 2]     += s;
            buffer[i * 2 + 1] += s;
        }
        engine->micPlaybackReadPos_ = rp + toRead;
    }

    SDL_PutAudioStreamData(stream, buffer, numFloats * sizeof(float));

    // Mono mixdown to output ring buffer for analysis
    for (int i = 0; i < numFrames; i++) {
        buffer[i] = (buffer[i * 2] + buffer[i * 2 + 1]) * 0.5f;
    }
    engine->outputBuffer_.write(buffer, numFrames);
}

void Engine::generateSamples(int numFrames, const BusList& buses)
{
    auto currentVoices = voices_.load();

    double baseTime = static_cast<double>(samplesGenerated_.load(std::memory_order_relaxed))
                      / static_cast<double>(sampleRate_);
    double sampleDt = 1.0 / static_cast<double>(sampleRate_);
    double endTime = baseTime + numFrames * sampleDt;

    // Drain scheduled events and apply them sample-accurately.
    // Events are sorted by the caller; we apply all events whose timestamp
    // falls within this callback's time window.
    {
        uint32_t r = eventRead_.load(std::memory_order_relaxed);
        uint32_t w = eventWrite_.load(std::memory_order_acquire);
        while (r != w) {
            auto& ev = eventRing_[r];
            if (ev.when > endTime) break; // future event, leave in queue

            // Find the voice and apply the trigger
            for (auto& v : *currentVoices) {
                if (v->id == ev.voiceId) {
                    if (ev.type == ScheduledEvent::Type::NoteOn) {
                        v->startTime.store(ev.when, std::memory_order_relaxed);
                        v->triggerStart.store(true, std::memory_order_release);
                    } else {
                        v->triggerRelease.store(true, std::memory_order_release);
                    }
                    break;
                }
            }

            r = (r + 1) % EVENT_RING_SIZE;
        }
        eventRead_.store(r, std::memory_order_release);
    }

    // Build a quick lookup: bus id → buffer pointer
    // Master bus is always present; unknown bus ids fall back to master
    float* masterBuf = nullptr;
    for (auto& bus : buses) {
        if (bus->id == MASTER_BUS_ID) { masterBuf = bus->buffer.data(); break; }
    }

    for (auto& voicePtr : *currentVoices) {
        Voice& voice = *voicePtr;

        if (voice.triggerStart.load(std::memory_order_acquire)) {
            voice.triggerStart.store(false, std::memory_order_relaxed);
            // Clear any pending release so a rapid noteOff+noteOn doesn't
            // immediately put the newly started voice into Release.
            voice.triggerRelease.store(false, std::memory_order_relaxed);
            if (voice.active && voice.started) {
                // Retrigger: voice is still sounding. Keep phase continuous
                // to avoid waveform discontinuity (click). Start attack from
                // current envelope level so there's no sudden amplitude jump.
                voice.envStage = EnvStage::Attack;
            } else {
                // Fresh start from silence.
                voice.started = true;
                voice.active = true;
                for (int u = 0; u < Voice::MAX_UNISON; u++)
                    voice.phases[u] = 0.0f;
                voice.envStage = EnvStage::Attack;
                voice.envLevel = 0.0f;
            }
        }

        if (voice.triggerRelease.load(std::memory_order_acquire)) {
            voice.triggerRelease.store(false, std::memory_order_relaxed);
            if (voice.envStage == EnvStage::Attack || voice.envStage == EnvStage::Decay
                || voice.envStage == EnvStage::Sustain) {
                voice.envStage = EnvStage::Release;
            }
        }

        if (!voice.active || !voice.started) continue;
        if (voice.envStage == EnvStage::Done) continue;

        // Find target bus buffer
        int targetBusId = voice.busId.load(std::memory_order_relaxed);
        float* buf = nullptr;
        for (auto& bus : buses) {
            if (bus->id == targetBusId) { buf = bus->buffer.data(); break; }
        }
        if (!buf) buf = masterBuf;
        if (!buf) continue;

        // Find send bus buffer (if configured)
        int sendId = voice.sendBusId.load(std::memory_order_relaxed);
        float baseSendAmt = voice.sendAmount.load(std::memory_order_relaxed);
        float* sendBuf = nullptr;
        if (sendId >= 0 && baseSendAmt > 0.0f) {
            for (auto& bus : buses) {
                if (bus->id == sendId) { sendBuf = bus->buffer.data(); break; }
            }
        }

        float baseFreq = voice.frequency.load(std::memory_order_relaxed);
        float gain = VOICE_AMPLITUDE * voice.gain.load(std::memory_order_relaxed);
        float pitchBendSemitones = voice.pitchBend.load(std::memory_order_relaxed);
        float basePan = voice.pan.load(std::memory_order_relaxed);
        float attRate = voice.attackRate.load(std::memory_order_relaxed);
        float decCoeff = voice.decayCoeff.load(std::memory_order_relaxed);
        float susLevel = voice.sustainLevel.load(std::memory_order_relaxed);
        float relCoeff = voice.releaseCoeff.load(std::memory_order_relaxed);
        double startTime = voice.startTime.load(std::memory_order_relaxed);
        Waveform wf = voice.waveform.load(std::memory_order_relaxed);

        // Load wavetable bank if needed (lock-free shared_ptr read)
        std::shared_ptr<const WavetableBank> wtBank;
        if (wf == Waveform::Wavetable) {
            wtBank = voice.wavetable.load(std::memory_order_acquire);
            if (!wtBank) continue;  // no wavetable set, skip this voice
        }

        bool isNoise = (wf == Waveform::WhiteNoise || wf == Waveform::PinkNoise
                        || wf == Waveform::BrownNoise);

        // Update unison cache if parameters changed
        int unisonN = voice.unisonCountCached;
        {
            uint32_t uv = voice.unisonVersion.load(std::memory_order_acquire);
            if (uv != voice.unisonVersionSeen) {
                voice.unisonVersionSeen = uv;
                unisonN = voice.unisonCount.load(std::memory_order_relaxed);
                voice.unisonCountCached = unisonN;
                float detune = voice.unisonDetune.load(std::memory_order_relaxed);
                float width = voice.unisonStereoWidth.load(std::memory_order_relaxed);
                for (int u = 0; u < unisonN; u++) {
                    if (unisonN > 1) {
                        float t = static_cast<float>(u) / static_cast<float>(unisonN - 1);
                        voice.unisonDetunes[u] = detune * (t - 0.5f);
                        voice.unisonPans[u] = width * (t * 2.0f - 1.0f);
                    } else {
                        voice.unisonDetunes[u] = 0.0f;
                        voice.unisonPans[u] = 0.0f;
                    }
                }
            }
        }
        float unisonGainNorm = 1.0f / std::sqrt(static_cast<float>(unisonN));

        for (int i = 0; i < numFrames; i++) {
            double t = baseTime + i * sampleDt;
            if (t < startTime) continue;

            switch (voice.envStage) {
                case EnvStage::Attack:
                    voice.envLevel += attRate;
                    if (voice.envLevel >= 1.0f) {
                        voice.envLevel = 1.0f;
                        voice.envStage = EnvStage::Decay;
                    }
                    break;
                case EnvStage::Decay:
                    voice.envLevel = susLevel + (voice.envLevel - susLevel) * decCoeff;
                    if (voice.envLevel - susLevel < 0.001f) {
                        voice.envLevel = susLevel;
                        voice.envStage = EnvStage::Sustain;
                    }
                    break;
                case EnvStage::Sustain:
                    voice.envLevel = susLevel;
                    break;
                case EnvStage::Release:
                    voice.envLevel *= relCoeff;
                    if (voice.envLevel < 0.0001f) {
                        voice.envLevel = 0.0f;
                        voice.envStage = EnvStage::Done;
                        voice.active = false;
                    }
                    break;
                case EnvStage::Done:
                case EnvStage::Idle:
                    break;
            }

            if (voice.envStage == EnvStage::Done) break;

            // Run modulation matrix for this sample
            ModValues mod;
            modMatrix_.process(mod, voice.modState, voice.envLevel, sampleRate_);

            // Apply pitch bend + mod pitch to frequency
            float totalPitchSemitones = pitchBendSemitones + mod.pitch;
            float freq = baseFreq * std::exp2(totalPitchSemitones / 12.0f);

            // Apply mod gain and pan
            float finalGain = gain * voice.envLevel * mod.gain;
            float finalPan = std::clamp(basePan + mod.pan, -1.0f, 1.0f);

            // Spatial override: if enabled, compute pan and attenuation from 3D position
            if (voice.spatial.spatialEnabled.load(std::memory_order_relaxed)) {
                float spatialPan = 0.0f;
                float spatialGain = computeSpatial(listener_, voice.spatial, spatialPan);
                finalGain *= spatialGain;
                finalPan = std::clamp(spatialPan + mod.pan, -1.0f, 1.0f);
            }

            // Generate sample(s) — unison sums N oscillators with per-osc pan
            float sumL = 0.0f, sumR = 0.0f;
            float monoSum = 0.0f;

            if (isNoise) {
                // Noise doesn't benefit from unison — generate once
                float s = generateNoise(wf, voice.noiseState);
                monoSum = s;
                float panL, panR;
                panGains(finalPan, panL, panR);
                sumL = s * panL;
                sumR = s * panR;
            } else {
                for (int u = 0; u < unisonN; u++) {
                    float uFreq = freq * std::exp2(voice.unisonDetunes[u] / 12.0f);
                    float uPhaseInc = uFreq / static_cast<float>(sampleRate_);

                    float s;
                    if (wf == Waveform::Wavetable) {
                        s = wtBank->sample(voice.phases[u], uPhaseInc);
                    } else {
                        s = generateSample(wf, voice.phases[u], uPhaseInc);
                    }

                    s *= unisonGainNorm;
                    monoSum += s;

                    float uPan = std::clamp(finalPan + voice.unisonPans[u], -1.0f, 1.0f);
                    float panL, panR;
                    panGains(uPan, panL, panR);
                    sumL += s * panL;
                    sumR += s * panR;

                    voice.phases[u] += uPhaseInc;
                    if (voice.phases[u] >= 1.0f) voice.phases[u] -= 1.0f;
                }
            }

            // Per-voice filter (modulated by mod matrix) — runs on mono sum
            if (voice.filter.enabled) {
                uint32_t fv = voice.filterVersion.load(std::memory_order_acquire);
                if (fv != voice.filterVersionSeen) {
                    voice.filterVersionSeen = fv;
                    voice.filter.enabled = voice.filterEnabled.load(std::memory_order_relaxed);
                    voice.filter.type = static_cast<BiquadFilter::Type>(
                        voice.filterType.load(std::memory_order_relaxed));
                    float baseCutoff = voice.filterFrequency.load(std::memory_order_relaxed);
                    float baseQ = voice.filterQ.load(std::memory_order_relaxed);
                    voice.filter.frequency = std::clamp(baseCutoff * mod.filterFreq, 20.0f, 20000.0f);
                    voice.filter.Q = std::clamp(baseQ * mod.filterQ, 0.1f, 30.0f);
                    voice.filter.computeCoefficients(sampleRate_);
                } else if (mod.filterFreq != 1.0f || mod.filterQ != 1.0f) {
                    float baseCutoff = voice.filterFrequency.load(std::memory_order_relaxed);
                    float baseQ = voice.filterQ.load(std::memory_order_relaxed);
                    voice.filter.frequency = std::clamp(baseCutoff * mod.filterFreq, 20.0f, 20000.0f);
                    voice.filter.Q = std::clamp(baseQ * mod.filterQ, 0.1f, 30.0f);
                    voice.filter.computeCoefficients(sampleRate_);
                }
                if (voice.filter.enabled) {
                    // Apply filter ratio to L/R (preserves stereo image)
                    float filteredMono = voice.filter.process(monoSum, 0);
                    float ratio = (monoSum != 0.0f) ? filteredMono / monoSum : 0.0f;
                    sumL *= ratio;
                    sumR *= ratio;
                }
            } else {
                // Check if filter was just enabled
                uint32_t fv = voice.filterVersion.load(std::memory_order_acquire);
                if (fv != voice.filterVersionSeen) {
                    voice.filterVersionSeen = fv;
                    voice.filter.enabled = voice.filterEnabled.load(std::memory_order_relaxed);
                    if (voice.filter.enabled) {
                        voice.filter.type = static_cast<BiquadFilter::Type>(
                            voice.filterType.load(std::memory_order_relaxed));
                        float baseCutoff = voice.filterFrequency.load(std::memory_order_relaxed);
                        float baseQ = voice.filterQ.load(std::memory_order_relaxed);
                        voice.filter.frequency = std::clamp(baseCutoff * mod.filterFreq, 20.0f, 20000.0f);
                        voice.filter.Q = std::clamp(baseQ * mod.filterQ, 0.1f, 30.0f);
                        voice.filter.computeCoefficients(sampleRate_);
                        voice.filter.reset();
                        float filteredMono = voice.filter.process(monoSum, 0);
                        float ratio = (monoSum != 0.0f) ? filteredMono / monoSum : 0.0f;
                        sumL *= ratio;
                        sumR *= ratio;
                    }
                }
            }

            buf[i * 2]     += sumL * finalGain;
            buf[i * 2 + 1] += sumR * finalGain;

            // Aux send (base amount + mod matrix delaySend)
            if (sendBuf) {
                float sendLevel = std::clamp(baseSendAmt + mod.delaySend, 0.0f, 1.0f);
                float sendGain = gain * voice.envLevel * sendLevel;
                sendBuf[i * 2]     += sumL * sendGain;
                sendBuf[i * 2 + 1] += sumR * sendGain;
            }
        }
    }

    // Clean up finished voices via RCU write (only if needed)
    // Purge one-shot voices that have finished their release envelope.
    // Persistent voices (managed by VoiceAllocator) are never purged — they
    // stay in the list for reuse.
    bool hasFinished = false;
    for (auto& v : *currentVoices) {
        if (v->started && (v->envStage == EnvStage::Done || !v->active)
            && !v->persistent.load(std::memory_order_relaxed)) {
            hasFinished = true;
            break;
        }
    }
    if (hasFinished) {
        std::unique_lock<std::mutex> lock(voiceWriteMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            auto freshVoices = voices_.load();
            auto newList = std::make_shared<VoiceList>();
            for (auto& v : *freshVoices) {
                bool finished = v->started && (v->envStage == EnvStage::Done || !v->active);
                bool persistent = v->persistent.load(std::memory_order_relaxed);
                if (!finished || persistent)
                    newList->push_back(v);
            }
            voices_.store(std::move(newList));
        }
    }

    samplesGenerated_.fetch_add(static_cast<uint64_t>(numFrames), std::memory_order_relaxed);
}

} // namespace broaudio
