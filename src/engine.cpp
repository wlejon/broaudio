#include "broaudio/engine.h"
#include "broaudio/synth/oscillator.h"
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

    delay_.init(sampleRate_ * 2);
    compressor_.init(sampleRate_);

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
// Voice management — RCU for the list, atomics for parameters
// ---------------------------------------------------------------------------

Voice* Engine::findVoice(int id)
{
    auto currentVoices = std::atomic_load(&voices_);
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

    auto newList = std::make_shared<VoiceList>(*std::atomic_load(&voices_));
    newList->push_back(voice);
    std::atomic_store(&voices_, std::const_pointer_cast<const VoiceList>(newList));

    return voice->id;
}

void Engine::removeVoice(int id)
{
    std::lock_guard<std::mutex> lock(voiceWriteMutex_);

    auto current = std::atomic_load(&voices_);
    auto newList = std::make_shared<VoiceList>();
    newList->reserve(current->size());
    for (auto& v : *current) {
        if (v->id != id) newList->push_back(v);
    }
    std::atomic_store(&voices_, std::const_pointer_cast<const VoiceList>(newList));
}

void Engine::setWaveform(int id, Waveform wf)
{
    if (auto* v = findVoice(id))
        v->waveform.store(wf, std::memory_order_relaxed);
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

void Engine::setMasterGain(float gain)
{
    masterGain_.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
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
// Filter control
// ---------------------------------------------------------------------------

int Engine::allocateFilterSlot()
{
    for (int i = 0; i < MAX_FILTERS; i++) {
        bool expected = false;
        if (filterParams_[i].allocated.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return i;
        }
    }
    return -1;
}

void Engine::releaseFilterSlot(int slot)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    filterParams_[slot].enabled.store(false, std::memory_order_relaxed);
    filterParams_[slot].allocated.store(false, std::memory_order_relaxed);
    filterParams_[slot].version.fetch_add(1, std::memory_order_release);
}

void Engine::setFilterEnabled(int slot, bool enabled)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    filterParams_[slot].enabled.store(enabled, std::memory_order_relaxed);
    filterParams_[slot].version.fetch_add(1, std::memory_order_release);
}

void Engine::setFilterType(int slot, BiquadFilter::Type type)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    filterParams_[slot].type.store(static_cast<int>(type), std::memory_order_relaxed);
    filterParams_[slot].version.fetch_add(1, std::memory_order_release);
}

void Engine::setFilterFrequency(int slot, float freq)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    filterParams_[slot].frequency.store(std::clamp(freq, 20.0f, 20000.0f), std::memory_order_relaxed);
    filterParams_[slot].version.fetch_add(1, std::memory_order_release);
}

void Engine::setFilterQ(int slot, float q)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    filterParams_[slot].Q.store(std::clamp(q, 0.1f, 30.0f), std::memory_order_relaxed);
    filterParams_[slot].version.fetch_add(1, std::memory_order_release);
}

void Engine::setFilterGain(int slot, float gainDB)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    filterParams_[slot].gainDB.store(std::clamp(gainDB, -40.0f, 40.0f), std::memory_order_relaxed);
    filterParams_[slot].version.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Delay control
// ---------------------------------------------------------------------------

void Engine::setDelayEnabled(bool enabled)
{
    delayParams_.enabled.store(enabled, std::memory_order_relaxed);
    delayParams_.version.fetch_add(1, std::memory_order_release);
}

void Engine::setDelayTime(float seconds)
{
    delayParams_.time.store(std::clamp(seconds, 0.001f, 2.0f), std::memory_order_relaxed);
    delayParams_.version.fetch_add(1, std::memory_order_release);
}

void Engine::setDelayFeedback(float fb)
{
    delayParams_.feedback.store(std::clamp(fb, 0.0f, 0.95f), std::memory_order_relaxed);
    delayParams_.version.fetch_add(1, std::memory_order_release);
}

void Engine::setDelayMix(float mix)
{
    delayParams_.mix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
    delayParams_.version.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Spatial
// ---------------------------------------------------------------------------

void Engine::setListenerPosition(float x, float y, float z)
{
    listener_.position = {x, y, z};
}

void Engine::setListenerOrientation(float fx, float fy, float fz,
                                     float ux, float uy, float uz)
{
    listener_.forward = {fx, fy, fz};
    listener_.up = {ux, uy, uz};
}

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
    float stackBuf[4096];
    float* buffer = (numSamples <= 4096) ? stackBuf : new float[numSamples];

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

    if (buffer != stackBuf) delete[] buffer;
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
// Audio Clips — RCU for clip and playback lists
// ---------------------------------------------------------------------------

AudioClip* Engine::findClip(int clipId) const
{
    auto currentClips = std::atomic_load(&clips_);
    for (auto& c : *currentClips) {
        if (c->id == clipId) return c.get();
    }
    return nullptr;
}

ClipPlayback* Engine::findPlayback(int instanceId) const
{
    auto currentPlaybacks = std::atomic_load(&playbacks_);
    for (auto& pb : *currentPlaybacks) {
        if (pb->id == instanceId && pb->active.load(std::memory_order_relaxed))
            return pb.get();
    }
    return nullptr;
}

int Engine::createClip(const float* samples, int numSamples)
{
    if (numSamples <= 0 || !samples) return -1;

    auto clip = std::make_shared<AudioClip>();
    std::lock_guard<std::mutex> lock(clipWriteMutex_);
    clip->id = nextClipId_++;
    clip->samples.assign(samples, samples + numSamples);

    int id = clip->id;
    auto newList = std::make_shared<ClipList>(*std::atomic_load(&clips_));
    newList->push_back(std::move(clip));
    std::atomic_store(&clips_, std::const_pointer_cast<const ClipList>(newList));

    return id;
}

void Engine::deleteClip(int clipId)
{
    std::lock_guard<std::mutex> lock(clipWriteMutex_);

    auto currentPB = std::atomic_load(&playbacks_);
    auto newPB = std::make_shared<PlaybackList>();
    for (auto& pb : *currentPB) {
        if (pb->clipId == clipId) {
            pb->playing.store(false, std::memory_order_relaxed);
            pb->active.store(false, std::memory_order_relaxed);
        } else {
            newPB->push_back(pb);
        }
    }
    std::atomic_store(&playbacks_, std::const_pointer_cast<const PlaybackList>(newPB));

    auto currentClips = std::atomic_load(&clips_);
    auto newClips = std::make_shared<ClipList>();
    for (auto& c : *currentClips) {
        if (c->id != clipId) newClips->push_back(c);
    }
    std::atomic_store(&clips_, std::const_pointer_cast<const ClipList>(newClips));
}

int Engine::getClipSampleCount(int clipId) const
{
    if (auto* c = findClip(clipId)) return static_cast<int>(c->samples.size());
    return 0;
}

void Engine::getClipWaveform(int clipId, float* outMinMax, int numBins) const
{
    auto* clip = findClip(clipId);
    if (!clip || clip->samples.empty()) {
        for (int i = 0; i < numBins * 2; i++) outMinMax[i] = 0.0f;
        return;
    }

    int totalSamples = static_cast<int>(clip->samples.size());
    float samplesPerBin = static_cast<float>(totalSamples) / static_cast<float>(numBins);

    for (int b = 0; b < numBins; b++) {
        int startIdx = static_cast<int>(b * samplesPerBin);
        int endIdx = static_cast<int>((b + 1) * samplesPerBin);
        endIdx = std::min(endIdx, totalSamples);

        float minVal = 1.0f, maxVal = -1.0f;
        for (int i = startIdx; i < endIdx; i++) {
            float s = clip->samples[i];
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
    std::lock_guard<std::mutex> lock(clipWriteMutex_);
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
    auto newList = std::make_shared<PlaybackList>(*std::atomic_load(&playbacks_));
    newList->push_back(std::move(pb));
    std::atomic_store(&playbacks_, std::const_pointer_cast<const PlaybackList>(newList));
    return id;
}

void Engine::stopPlayback(int instanceId)
{
    std::lock_guard<std::mutex> lock(clipWriteMutex_);
    auto current = std::atomic_load(&playbacks_);
    auto newList = std::make_shared<PlaybackList>();
    for (auto& pb : *current) {
        if (pb->id == instanceId) {
            pb->playing.store(false, std::memory_order_relaxed);
            pb->active.store(false, std::memory_order_relaxed);
        } else {
            newList->push_back(pb);
        }
    }
    std::atomic_store(&playbacks_, std::const_pointer_cast<const PlaybackList>(newList));
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
        pb->rate.store(rate, std::memory_order_relaxed);
}

void Engine::setPlaybackPan(int instanceId, float pan)
{
    if (auto* pb = findPlayback(instanceId))
        pb->pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void Engine::setPlaybackRegion(int instanceId, int start, int end)
{
    std::lock_guard<std::mutex> lock(clipWriteMutex_);
    if (auto* pb = findPlayback(instanceId)) {
        auto* clip = findClip(pb->clipId);
        if (!clip) return;
        int maxLen = static_cast<int>(clip->samples.size());
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
    int end = re > 0 ? re : static_cast<int>(clip->samples.size());
    int len = end - rs;
    if (len <= 0) return 0.0f;
    uint64_t pos = pb->playPos.load(std::memory_order_relaxed);
    int intPos = static_cast<int>(pos >> 16);
    return static_cast<float>(intPos % len) / static_cast<float>(len);
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

    float stackBuf[8192];
    float* buffer = (numFloats <= 8192) ? stackBuf : new float[numFloats];

    std::memset(buffer, 0, numFloats * sizeof(float));
    engine->generateSamples(buffer, numFrames);

    // Record tap (mono mixdown, before effects)
    if (engine->recording_.load(std::memory_order_relaxed)) {
        uint64_t wp = engine->recordWritePos_.load(std::memory_order_relaxed);
        for (int i = 0; i < numFrames; i++) {
            float mono = (buffer[i * 2] + buffer[i * 2 + 1]) * 0.5f;
            engine->recordRing_[static_cast<int>((wp + i) % RECORD_RING_SIZE)] = mono;
        }
        engine->recordWritePos_.store(wp + numFrames, std::memory_order_release);
    }

    // Mix clip playback instances (lock-free RCU read)
    {
        auto currentClips = std::atomic_load(&engine->clips_);
        auto currentPlaybacks = std::atomic_load(&engine->playbacks_);
        for (auto& pb : *currentPlaybacks) {
            if (!pb->active.load(std::memory_order_relaxed)) continue;
            if (!pb->playing.load(std::memory_order_relaxed)) continue;

            AudioClip* clip = nullptr;
            for (auto& c : *currentClips) {
                if (c->id == pb->clipId) { clip = c.get(); break; }
            }
            if (!clip) continue;

            int start = pb->regionStart.load(std::memory_order_relaxed);
            int end = pb->regionEnd.load(std::memory_order_relaxed);
            end = end > 0 ? end : static_cast<int>(clip->samples.size());
            int len = end - start;
            if (len <= 0) continue;

            uint64_t pos = pb->playPos.load(std::memory_order_relaxed);
            float g = pb->gain.load(std::memory_order_relaxed);
            float rate = pb->rate.load(std::memory_order_relaxed);
            bool looping = pb->looping.load(std::memory_order_relaxed);
            float panL, panR;
            panGains(pb->pan.load(std::memory_order_relaxed), panL, panR);

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
                float s0 = clip->samples[start + intPos];
                int nextIdx = intPos + 1;
                if (nextIdx >= len) nextIdx = looping ? 0 : intPos;
                float s1 = clip->samples[start + nextIdx];
                float sample = (s0 + frac * (s1 - s0)) * g;
                buffer[i * 2]     += sample * panL;
                buffer[i * 2 + 1] += sample * panR;
                pos += increment;
            }
            pb->playPos.store(pos, std::memory_order_relaxed);
        }
    }

    // Apply filters
    for (int f = 0; f < Engine::MAX_FILTERS; f++) {
        uint32_t ver = engine->filterParams_[f].version.load(std::memory_order_acquire);
        if (ver != engine->filterVersions_[f]) {
            engine->filterVersions_[f] = ver;
            bool enabled = engine->filterParams_[f].enabled.load(std::memory_order_relaxed);
            engine->filters_[f].enabled = enabled;
            if (enabled) {
                engine->filters_[f].type = static_cast<BiquadFilter::Type>(
                    engine->filterParams_[f].type.load(std::memory_order_relaxed));
                engine->filters_[f].frequency = engine->filterParams_[f].frequency.load(std::memory_order_relaxed);
                engine->filters_[f].Q = engine->filterParams_[f].Q.load(std::memory_order_relaxed);
                engine->filters_[f].gainDB = engine->filterParams_[f].gainDB.load(std::memory_order_relaxed);
                engine->filters_[f].computeCoefficients(engine->sampleRate_);
            } else {
                engine->filters_[f].reset();
            }
        }
        if (!engine->filters_[f].enabled) continue;
        for (int i = 0; i < numFrames; i++) {
            buffer[i * 2]     = engine->filters_[f].process(buffer[i * 2], 0);
            buffer[i * 2 + 1] = engine->filters_[f].process(buffer[i * 2 + 1], 1);
        }
    }

    // Apply delay
    {
        uint32_t ver = engine->delayParams_.version.load(std::memory_order_acquire);
        if (ver != engine->delayVersion_) {
            engine->delayVersion_ = ver;
            engine->delay_.enabled = engine->delayParams_.enabled.load(std::memory_order_relaxed);
            float delaySec = engine->delayParams_.time.load(std::memory_order_relaxed);
            int maxSamples = static_cast<int>(engine->delay_.buffer.size());
            engine->delay_.delaySamples = std::clamp(
                static_cast<int>(delaySec * engine->sampleRate_), 1, maxSamples - 1);
            engine->delay_.feedback = engine->delayParams_.feedback.load(std::memory_order_relaxed);
            engine->delay_.mix = engine->delayParams_.mix.load(std::memory_order_relaxed);
        }
        if (engine->delay_.enabled) {
            engine->delay_.processStereo(buffer, numFrames);
        }
    }

    // Compressor
    engine->compressor_.processStereo(buffer, numFrames);

    // Soft limiter + master gain
    float mg = engine->masterGain_.load(std::memory_order_relaxed);
    for (int i = 0; i < numFloats; i++) {
        buffer[i] = softLimit(buffer[i]) * mg;
    }

    // Mix mic monitor
    if (!engine->micMuted_.load(std::memory_order_relaxed)) {
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
    float monoBuf[4096];
    int monoCount = std::min(numFrames, 4096);
    for (int i = 0; i < monoCount; i++) {
        monoBuf[i] = (buffer[i * 2] + buffer[i * 2 + 1]) * 0.5f;
    }
    engine->outputBuffer_.write(monoBuf, monoCount);

    if (buffer != stackBuf) delete[] buffer;
}

void Engine::generateSamples(float* buffer, int numFrames)
{
    auto currentVoices = std::atomic_load(&voices_);

    double baseTime = static_cast<double>(samplesGenerated_.load(std::memory_order_relaxed))
                      / static_cast<double>(sampleRate_);
    double sampleDt = 1.0 / static_cast<double>(sampleRate_);

    for (auto& voicePtr : *currentVoices) {
        Voice& voice = *voicePtr;

        if (voice.triggerStart.load(std::memory_order_acquire)) {
            voice.triggerStart.store(false, std::memory_order_relaxed);
            voice.started = true;
            voice.active = true;
            voice.phase = 0.0f;
            voice.envStage = EnvStage::Attack;
            voice.envLevel = 0.0f;
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

        float freq = voice.frequency.load(std::memory_order_relaxed);
        float gain = VOICE_AMPLITUDE * voice.gain.load(std::memory_order_relaxed);
        float phaseInc = freq / static_cast<float>(sampleRate_);
        float panL, panR;
        panGains(voice.pan.load(std::memory_order_relaxed), panL, panR);
        float attRate = voice.attackRate.load(std::memory_order_relaxed);
        float decCoeff = voice.decayCoeff.load(std::memory_order_relaxed);
        float susLevel = voice.sustainLevel.load(std::memory_order_relaxed);
        float relCoeff = voice.releaseCoeff.load(std::memory_order_relaxed);
        double startTime = voice.startTime.load(std::memory_order_relaxed);
        Waveform wf = voice.waveform.load(std::memory_order_relaxed);

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

            float sample = generateSample(wf, voice.phase, phaseInc);
            float s = sample * gain * voice.envLevel;
            buffer[i * 2]     += s * panL;
            buffer[i * 2 + 1] += s * panR;

            voice.phase += phaseInc;
            if (voice.phase >= 1.0f) voice.phase -= 1.0f;
        }
    }

    // Clean up finished voices via RCU write (only if needed)
    bool hasFinished = false;
    for (auto& v : *currentVoices) {
        if (v->started && (v->envStage == EnvStage::Done || !v->active)) {
            hasFinished = true;
            break;
        }
    }
    if (hasFinished) {
        std::unique_lock<std::mutex> lock(voiceWriteMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            auto newList = std::make_shared<VoiceList>();
            for (auto& v : *currentVoices) {
                if (!(v->started && (v->envStage == EnvStage::Done || !v->active)))
                    newList->push_back(v);
            }
            std::atomic_store(&voices_, std::const_pointer_cast<const VoiceList>(newList));
        }
    }

    samplesGenerated_.fetch_add(static_cast<uint64_t>(numFrames), std::memory_order_relaxed);
}

} // namespace broaudio
