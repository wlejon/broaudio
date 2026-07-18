#include "broaudio/engine.h"
#include "broaudio/clip/file_stream.h"
#include "broaudio/io/audio_stream.h"
#include "broaudio/log.h"
#include "broaudio/synth/oscillator.h"
#include "broaudio/synth/wavetable.h"
#include "broaudio/dsp/equalizer.h"
#include "broaudio/dsp/fft.h"
#include "broaudio/dsp/limiter.h"
#include "broaudio/dsp/resampler.h"

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
        log(LogLevel::Error, "broaudio: Failed to init SDL audio: %s", SDL_GetError());
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
        log(LogLevel::Error, "broaudio: Failed to open audio device: %s", SDL_GetError());
        return false;
    }

    // Output latency estimate: device buffer frames / device rate. Only the
    // SDL buffer is visible from here (see outputLatencySeconds() docs).
    {
        SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream_);
        SDL_AudioSpec devSpec{};
        int devFrames = 0;
        if (dev != 0 && SDL_GetAudioDeviceFormat(dev, &devSpec, &devFrames)
            && devFrames > 0 && devSpec.freq > 0) {
            outputLatencySeconds_ =
                static_cast<double>(devFrames) / static_cast<double>(devSpec.freq);
        }
    }

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

    // Initialize master gain smoother
    smoothMasterGain_.init(sampleRate_);
    smoothMasterGain_.snap(masterGain_.load(std::memory_order_relaxed));

    // Resume audio *after* all buffers and buses are ready — starting earlier
    // would let the audio callback race with init and corrupt the heap.
    SDL_ResumeAudioStreamDevice(stream_);

    initialized_ = true;
    log(LogLevel::Info, "broaudio: initialized %d Hz stereo", sampleRate_);
    return true;
}

bool Engine::initHeadless()
{
    if (initialized_) return true;

    // Create master bus (id 0) — same as init() but no SDL audio device
    {
        auto master = std::make_shared<Bus>();
        master->id = MASTER_BUS_ID;
        master->parentId.store(-1, std::memory_order_relaxed);
        master->initAudioState(sampleRate_, MAX_SCRATCH_FRAMES);

        auto list = std::make_shared<BusList>();
        list->push_back(std::move(master));
        buses_.store(std::move(list));
    }

    outputScratch_.resize(MAX_SCRATCH_FRAMES * 2, 0.0f);
    micScratch_.resize(MAX_SCRATCH_FRAMES, 0.0f);

    // Initialize master gain smoother
    smoothMasterGain_.init(sampleRate_);
    smoothMasterGain_.snap(masterGain_.load(std::memory_order_relaxed));

    initialized_ = true;
    log(LogLevel::Info, "broaudio: initialized headless %d Hz stereo (no audio device)", sampleRate_);
    return true;
}

void Engine::renderBlock(int numFrames)
{
    if (!initialized_ || numFrames <= 0) return;

    // Master pause suspends the offline clock too — the host keeps calling
    // renderBlock on its own cadence, but nothing renders or advances, same
    // freeze-in-place semantics as the realtime callback.
    if (masterPaused_.load(std::memory_order_relaxed)) return;

    // Reader scope for host-driven (non-SDL) rendering — covers
    // renderInternal / generateSamples / processBusEffects transitively.
    RcuDomain::ReadScope rcuScope(rcu_);

    // Process in chunks up to scratch buffer size
    while (numFrames > 0) {
        int chunk = std::min(numFrames, MAX_SCRATCH_FRAMES);
        renderInternal(chunk);
        numFrames -= chunk;
    }
}

void Engine::renderInternal(int numFrames)
{
    // numFrames is bounded by MAX_SCRATCH_FRAMES (renderBlock chunks above
    // us), so outputScratch_ — pre-sized in init — is always large enough.
    float* buffer = outputScratch_.data();

    auto currentBuses = buses_.load();

    for (auto& bus : *currentBuses) {
        int frames = std::min(numFrames, static_cast<int>(bus->buffer.size()) / 2);
        bus->clearBuffer(frames);
    }

    generateSamples(numFrames, *currentBuses);

    // Mix clip playback into target bus buffers
    {
        auto currentClips = clips_.load();
        auto currentPlaybacks = playbacks_.load();
        // This block spans absolute samples [blockStart, blockStart+numFrames).
        // generateSamples() above already advanced samplesGenerated_ by numFrames
        // (same as the realtime path), so the counter holds the block's END sample.
        uint64_t blockEnd = samplesGenerated_.load(std::memory_order_relaxed);
        uint64_t blockStart = blockEnd >= static_cast<uint64_t>(numFrames)
                                  ? blockEnd - static_cast<uint64_t>(numFrames) : 0;
        for (auto& pb : *currentPlaybacks) {
            if (!pb->active.load(std::memory_order_relaxed)) continue;
            if (!pb->playing.load(std::memory_order_relaxed)) continue;

            AudioClip* clip = nullptr;
            for (auto& c : *currentClips) {
                if (c->id == pb->clipId) { clip = c.get(); break; }
            }
            if (!clip) continue;

            int targetBusId = pb->busId.load(std::memory_order_relaxed);
            float* targetBuf = nullptr;
            for (auto& bus : *currentBuses) {
                if (bus->id == targetBusId) { targetBuf = bus->buffer.data(); break; }
            }
            if (!targetBuf) {
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
            float rate = pb->rate.load(std::memory_order_relaxed);
            bool looping = pb->looping.load(std::memory_order_relaxed);
            int ch = clip->channels;

            // Set smoother targets for gain and pan
            float targetGain = pb->gain.load(std::memory_order_relaxed);
            float targetPan = pb->pan.load(std::memory_order_relaxed);

            HeadParams headParams;
            bool spatialFilterActive = false;
            if (pb->spatial.spatialEnabled.load(std::memory_order_relaxed)) {
                auto sr = computeSpatial(listener_, pb->spatial);
                targetGain *= sr.gain;
                targetPan = 0.0f; // center — head model does L/R
                headParams = computeHeadParams(sr, headModel_, sampleRate_);
                spatialFilterActive = true;
            }

            pb->smoothGain.set(targetGain);
            pb->smoothPan.set(targetPan);

            int clipSendId = pb->sendBusId.load(std::memory_order_relaxed);
            float clipSendAmt = pb->sendAmount.load(std::memory_order_relaxed);
            float* clipSendBuf = nullptr;
            if (clipSendId >= 0 && clipSendAmt > 0.0f) {
                for (auto& bus : *currentBuses) {
                    if (bus->id == clipSendId) { clipSendBuf = bus->buffer.data(); break; }
                }
            }

            // Streaming sources read from a ring instead of a fixed clip.
            if (clip->streaming) {
                mixStreamPlayback(pb.get(), clip, targetBuf, clipSendBuf, clipSendAmt,
                                  spatialFilterActive, headParams, numFrames, 0);
                continue;
            }

            constexpr int FRAC_BITS = 16;
            constexpr uint64_t FRAC_MASK = (1ULL << FRAC_BITS) - 1;
            uint64_t increment = static_cast<uint64_t>(rate * (1 << FRAC_BITS) + 0.5f);

            // Sample-accurate scheduled start (see the realtime path for the rationale):
            // stay silent until the audio clock reaches startSample, then begin mid-block.
            int startFrame = 0;
            uint64_t startS = pb->startSample.load(std::memory_order_relaxed);
            if (startS > blockStart) {
                uint64_t off = startS - blockStart;
                if (off >= static_cast<uint64_t>(numFrames)) continue;  // starts in a later block
                startFrame = static_cast<int>(off);
            }

            for (int i = startFrame; i < numFrames; i++) {
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

                float g = pb->smoothGain.next();
                float clipPan = pb->smoothPan.next();
                float panL, panR;
                panGains(clipPan, panL, panR);

                float outL, outR;
                if (ch == 2) {
                    int idx0 = (start + intPos) * 2;
                    int idx1 = (start + nextIdx) * 2;
                    float L0 = clip->samples[idx0];
                    float R0 = clip->samples[idx0 + 1];
                    float L1 = clip->samples[idx1];
                    float R1 = clip->samples[idx1 + 1];
                    float sL = (L0 + frac * (L1 - L0)) * g;
                    float sR = (R0 + frac * (R1 - R0)) * g;
                    outL = sL * panL + sR * (1.0f - panR);
                    outR = sR * panR + sL * (1.0f - panL);
                } else {
                    float s0 = clip->samples[start + intPos];
                    float s1 = clip->samples[start + nextIdx];
                    float sample = (s0 + frac * (s1 - s0)) * g;
                    outL = sample * panL;
                    outR = sample * panR;
                }

                if (spatialFilterActive)
                    pb->spatialFilter.process(outL, outR, headParams);

                targetBuf[i * 2]     += outL;
                targetBuf[i * 2 + 1] += outR;

                if (clipSendBuf) {
                    clipSendBuf[i * 2]     += outL * clipSendAmt;
                    clipSendBuf[i * 2 + 1] += outR * clipSendAmt;
                }

                pos += increment;
            }
            pb->playPos.store(pos, std::memory_order_relaxed);
        }
    }

    // Locate the master bus for the bus-graph mixdown below. The record tap
    // happens after the master limiter (further down) so it captures voices
    // routed through user-created buses, not just voices that bypassed the
    // bus tree.
    Bus* masterBus = nullptr;
    for (auto& bus : *currentBuses) {
        if (bus->id == MASTER_BUS_ID) { masterBus = bus.get(); break; }
    }

    // Process child buses
    for (auto& bus : *currentBuses) {
        if (bus->id == MASTER_BUS_ID) continue;

        processBusEffects(*bus, numFrames);

        // Solo gate: a silenced bus contributes neither to its parent nor to
        // its aux send (its effect state above still ran, so un-soloing is
        // click-free and tails stay warm).
        bool audible = busAudibleUnderSolo(*currentBuses, *bus);

        int parentId = bus->parentId.load(std::memory_order_relaxed);
        for (auto& parent : *currentBuses) {
            if (parent->id == parentId) {
                if (audible) mixBusIntoParent(*bus, *parent, numFrames);
                break;
            }
        }

        int busSendId = bus->sendBusId.load(std::memory_order_relaxed);
        float busSendAmt = bus->sendAmount.load(std::memory_order_relaxed);
        if (audible && busSendId >= 0 && busSendAmt > 0.0f) {
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
        processBusEffects(*masterBus, numFrames);
    }

    // Apply master gain (smoothed) + limiter
    if (masterBus) {
        smoothMasterGain_.set(masterGain_.load(std::memory_order_relaxed));
        for (int i = 0; i < numFrames; i++) {
            float mg = smoothMasterGain_.next();
            buffer[i * 2]     = masterBus->buffer[i * 2]     * mg;
            buffer[i * 2 + 1] = masterBus->buffer[i * 2 + 1] * mg;
        }
        masterLimiter_.process(buffer, static_cast<size_t>(numFrames));
    } else {
        std::memset(buffer, 0, numFrames * 2 * sizeof(float));
    }

    // Record tap (mono mixdown of final stereo output). Lives here, after
    // the bus graph + master gain + limiter, so it captures voices routed
    // through user buses — not just voices on the master bus.
    if (recording_.load(std::memory_order_relaxed)) {
        uint64_t wp = recordWritePos_.load(std::memory_order_relaxed);
        for (int i = 0; i < numFrames; i++) {
            float mono = (buffer[i * 2] + buffer[i * 2 + 1]) * 0.5f;
            recordRing_[static_cast<int>((wp + i) % RECORD_RING_SIZE)] = mono;
        }
        recordWritePos_.store(wp + numFrames, std::memory_order_release);
    }

    // Mono mixdown to output ring buffer for analysis
    for (int i = 0; i < numFrames; i++) {
        buffer[i] = (buffer[i * 2] + buffer[i * 2 + 1]) * 0.5f;
    }
    outputBuffer_.write(buffer, numFrames);

    // NOTE: samplesGenerated_ is advanced by generateSamples() above (the single
    // source of truth, shared with the realtime processOutputChunk path). The
    // extra fetch_add that used to live here double-counted the offline clock —
    // currentTime() ran at 2x in headless/renderBlock, and scheduled-event /
    // clip-start times landed at half their intended sample. Removed.
}

void Engine::shutdown()
{
    // Stop disk-stream decode workers before tearing the device down —
    // they hold clip/playback refs and an SDL_AudioStream resampler each.
    stopAllFileStreams();
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

// --- Per-bus distortion/waveshaper control ---

void Engine::setBusDistortionEnabled(int busId, bool enabled)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.enabled.store(enabled, std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
}

void Engine::setBusDistortionMode(int busId, DistortionMode mode)
{
    auto* bus = findBus(busId);
    if (!bus) return;
    bus->distortionParams.mode.store(static_cast<int>(mode), std::memory_order_relaxed);
    bus->distortionParams.version.fetch_add(1, std::memory_order_release);
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
    if (auto* v = findVoice(id)) {
        v->startTime.store(when, std::memory_order_relaxed);
        v->triggerStart.store(true, std::memory_order_release);
    }
}

void Engine::stopVoice(int id, double when)
{
    if (when > currentTime()) {
        scheduleNoteOff(id, when);
    } else if (auto* v = findVoice(id)) {
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
        log(LogLevel::Error, "broaudio: Failed to open mic device: %s", SDL_GetError());
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
        engine->processMicSamples(buffer, samplesGot);
    }
}

void Engine::processMicSamples(const float* buffer, int samplesGot)
{
    if (samplesGot <= 0) return;

    // Fan out to multi-consumer taps first, before the analysis ring / monitor
    // bookkeeping, so wake-word and ASR consumers see samples with minimum
    // latency. Runs under one RCU read scope.
    {
        RcuDomain::ReadScope rcuScope(rcu_);
        dispatchMicTaps(buffer, samplesGot);
    }

    // Analysis ring — SPSC-safe via AnalysisBuffer's atomic writePos_.
    micBuffer_.write(buffer, samplesGot);

    // Monitor FIFO (audio thread → output mixer).
    int cap = MIC_FIFO_SIZE;
    uint64_t wp = micPlaybackWritePos_.load(std::memory_order_relaxed);
    for (int i = 0; i < samplesGot; i++) {
        micPlayback_[static_cast<int>((wp + i) % cap)] = buffer[i];
    }
    micPlaybackWritePos_.store(wp + samplesGot, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Mic taps (multi-consumer dispatch with per-tap resample + AGC + chunking)
// ---------------------------------------------------------------------------

void Engine::injectMicSamples(const float* samples, int numSamples)
{
    if (numSamples <= 0 || !samples) return;
    // Tap dispatch only — no analysis-ring or monitor-FIFO writes, so this
    // never races the SPSC publication the recording callback relies on. The
    // caller guarantees no live capture is running concurrently (see header).
    RcuDomain::ReadScope rcuScope(rcu_);
    dispatchMicTaps(samples, numSamples);
}

Engine::MicTap::~MicTap()
{
    if (resampler) {
        SDL_DestroyAudioStream(resampler);
        resampler = nullptr;
    }
}

MicTapId Engine::addMicTap(const MicTapConfig& cfg, MicTapCallback cb)
{
    if (!cb) return kInvalidMicTapId;
    if (cfg.targetRate < 0 || cfg.chunkFrames < 0) return kInvalidMicTapId;

    auto tap = std::make_shared<MicTap>();
    tap->cfg      = cfg;
    tap->callback = std::move(cb);
    tap->agc.setConfig(cfg.agcCfg);

    const int engineRate    = sampleRate_;
    const bool needResample =
        (cfg.targetRate > 0 && cfg.targetRate != engineRate);
    tap->effectiveRate = needResample ? cfg.targetRate : engineRate;

    if (needResample) {
        SDL_AudioSpec src{}, dst{};
        src.format   = SDL_AUDIO_F32;
        src.channels = 1;
        src.freq     = engineRate;
        dst.format   = SDL_AUDIO_F32;
        dst.channels = 1;
        dst.freq     = cfg.targetRate;
        tap->resampler = SDL_CreateAudioStream(&src, &dst);
        if (!tap->resampler) {
            log(LogLevel::Error,
                "broaudio: addMicTap: SDL_CreateAudioStream failed: %s",
                SDL_GetError());
            return kInvalidMicTapId;
        }
    }

    tap->id = nextMicTapId_++;

    // COW publish: copy the current list, append, store.
    auto current = micTaps_.load();
    auto next = std::make_shared<MicTapList>(*current);
    next->push_back(tap);
    micTaps_.store(std::shared_ptr<const MicTapList>(std::move(next)));
    return tap->id;
}

void Engine::removeMicTap(MicTapId id)
{
    if (id == kInvalidMicTapId) return;
    auto current = micTaps_.load();
    auto next = std::make_shared<MicTapList>();
    next->reserve(current->size());
    for (auto& t : *current) {
        if (t->id != id) next->push_back(t);
    }
    if (next->size() == current->size()) return;  // not found
    micTaps_.store(std::shared_ptr<const MicTapList>(std::move(next)));
    // The removed tap's shared_ptr drops off the list here. Any audio-thread
    // callback that captured an old snapshot still holds it through the
    // ReadScope; once that scope ends and QSBR observes a quiescent state,
    // the MicTap destructor runs and frees the SDL_AudioStream.
}

MicTapStats Engine::getMicTapStats(MicTapId id) const
{
    MicTapStats stats{};
    if (id == kInvalidMicTapId) return stats;
    auto list = micTaps_.load();
    for (auto& t : *list) {
        if (t->id != id) {
            continue;
        }
        stats.framesDelivered  = t->framesDelivered.load(std::memory_order_relaxed);
        stats.samplesDelivered = t->samplesDelivered.load(std::memory_order_relaxed);
        stats.rollingPeak      = t->rollingPeakX10000.load(std::memory_order_relaxed) / 10000.0f;
        break;
    }
    return stats;
}

void Engine::dispatchMicTaps(const float* samples, int numSamples)
{
    auto list = micTaps_.load(std::memory_order_acquire);
    if (!list || list->empty()) return;
    for (auto& tap : *list) {
        deliverTapChunk(*tap, samples, numSamples);
    }
}

void Engine::deliverTapChunk(MicTap& tap, const float* samples, int numSamples)
{
    // Stage 1: resample into a tap-local buffer (or pass-through).
    const float* feed = samples;
    int          feedN = numSamples;

    if (tap.resampler) {
        const int putBytes = numSamples * static_cast<int>(sizeof(float));
        SDL_PutAudioStreamData(tap.resampler, samples, putBytes);
        const int availBytes = SDL_GetAudioStreamAvailable(tap.resampler);
        if (availBytes <= 0) return;
        const int availSamples = availBytes / static_cast<int>(sizeof(float));
        if (static_cast<int>(tap.scratch.size()) < availSamples) {
            tap.scratch.resize(static_cast<std::size_t>(availSamples));
        }
        const int gotBytes = SDL_GetAudioStreamData(
            tap.resampler, tap.scratch.data(), availBytes);
        if (gotBytes <= 0) return;
        feed = tap.scratch.data();
        feedN = gotBytes / static_cast<int>(sizeof(float));
    }

    // Stage 2: AGC (in-place — promote pass-through samples to scratch first
    // so we never mutate the caller's buffer).
    if (tap.cfg.agc) {
        if (feed != tap.scratch.data()) {
            if (static_cast<int>(tap.scratch.size()) < feedN) {
                tap.scratch.resize(static_cast<std::size_t>(feedN));
            }
            std::memcpy(tap.scratch.data(), feed,
                        static_cast<std::size_t>(feedN) * sizeof(float));
            feed = tap.scratch.data();
        }
        tap.agc.apply(tap.scratch.data(), feedN, tap.effectiveRate);
    }

    // Stage 3: dispatch in chunkFrames-sized slices, or one shot.
    auto recordStats = [&tap](const float* s, int n) {
        float p = 0.0f;
        for (int i = 0; i < n; ++i) {
            float a = std::fabs(s[i]);
            if (a > p) p = a;
        }
        int pk = static_cast<int>(p * 10000.0f);
        int prev = tap.rollingPeakX10000.load(std::memory_order_relaxed);
        while (pk > prev &&
               !tap.rollingPeakX10000.compare_exchange_weak(prev, pk)) {}
        tap.framesDelivered.fetch_add(1, std::memory_order_relaxed);
        tap.samplesDelivered.fetch_add(
            static_cast<std::uint64_t>(n), std::memory_order_relaxed);
    };

    if (tap.cfg.chunkFrames <= 0) {
        recordStats(feed, feedN);
        tap.callback(feed, feedN);
        return;
    }

    const int chunk = tap.cfg.chunkFrames;
    // Append to the tap's chunk buffer, then emit complete chunks.
    const std::size_t prevSize = tap.chunkBuf.size();
    tap.chunkBuf.resize(prevSize + static_cast<std::size_t>(feedN));
    std::memcpy(tap.chunkBuf.data() + prevSize, feed,
                static_cast<std::size_t>(feedN) * sizeof(float));

    std::size_t offset = 0;
    while (tap.chunkBuf.size() - offset >= static_cast<std::size_t>(chunk)) {
        const float* p = tap.chunkBuf.data() + offset;
        recordStats(p, chunk);
        tap.callback(p, chunk);
        offset += static_cast<std::size_t>(chunk);
    }
    if (offset > 0) {
        const std::size_t remaining = tap.chunkBuf.size() - offset;
        std::memmove(tap.chunkBuf.data(),
                     tap.chunkBuf.data() + offset,
                     remaining * sizeof(float));
        tap.chunkBuf.resize(remaining);
    }
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void Engine::startRecording()
{
    recordStartPos_.store(recordWritePos_.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    recording_.store(true, std::memory_order_release);
}

void Engine::stopRecording()
{
    recording_.store(false, std::memory_order_release);

    uint64_t endPos = recordWritePos_.load(std::memory_order_acquire);
    uint64_t startPos = recordStartPos_.load(std::memory_order_relaxed);
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

    // numBins is the count of magnitude bins to fill, all spanning
    // [0, Nyquist] — i.e. the Web Audio convention where bin k represents
    // frequency k * sampleRate / fftSize for an FFT of size 2 * numBins.
    // Choose the smallest power-of-2 FFT that yields at least numBins of
    // unique magnitudes.
    int n = 1;
    while (n < numBins * 2) n <<= 1;
    if (n > 16384) n = 16384;

    std::vector<float> real(n, 0.0f);
    std::vector<float> imag(n, 0.0f);

    outputBuffer_.readLatest(real.data(), n);

    // Apply Hann window
    for (int i = 0; i < n; i++) {
        float w = 0.5f * (1.0f - std::cos(2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(n)));
        real[i] *= w;
    }

    fft(real.data(), imag.data(), n);

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

int Engine::playClipAt(int clipId, double when, float gain, bool loop)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    if (!findClip(clipId)) return -1;

    // Resolve the start to an absolute engine sample. A time at/before now maps
    // to 0 (immediate); the mixer treats 0 / past samples as "already started".
    uint64_t startSample = 0;
    double s = when * static_cast<double>(sampleRate_);
    if (s > 0.0) startSample = static_cast<uint64_t>(s + 0.5);

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
    pb->startSample.store(startSample, std::memory_order_relaxed);

    int id = pb->id;
    auto newList = std::make_shared<PlaybackList>(*playbacks_.load());
    newList->push_back(std::move(pb));
    playbacks_.store(std::move(newList));
    return id;
}

// Ring-read mix for a streaming playback. Caller has already set the gain/pan
// smoother targets and resolved targetBuf / sends / spatial; this just reads the
// ring and applies the same per-frame gain/pan/spatial/write as the clip path.
void Engine::mixStreamPlayback(ClipPlayback* pb, AudioClip* clip, float* targetBuf,
                               float* clipSendBuf, float clipSendAmt,
                               bool spatialFilterActive, const HeadParams& headParams,
                               int numFrames, int startFrame)
{
    const int cap = clip->ringFrames;
    const int ch = clip->channels;
    if (cap <= 0) return;

    uint64_t wf = clip->writeFrames.load(std::memory_order_acquire);
    uint64_t rf = pb->playPos.load(std::memory_order_relaxed);
    // Seek fence: a disk-stream seek publishes the writeFrames value at the
    // moment of the seek; everything below it is pre-seek audio the worker
    // could not un-push. Fast-forward past it (silence until the worker
    // refills from the new file position).
    uint64_t ff = clip->streamFlushFrames.load(std::memory_order_acquire);
    if (ff > rf) rf = ff;
    // Overrun: if the reader has fallen more than a full ring behind, jump
    // forward (drop the oldest audio) so latency stays bounded.
    if (wf > rf + static_cast<uint64_t>(cap))
        rf = wf - static_cast<uint64_t>(cap) / 2;

    // Starvation accounting: silent frames emitted while the producer is
    // still expected to deliver (i.e. the stream hasn't ended). Batched into
    // one relaxed add after the loop — never a lock, never a syscall.
    int underruns = 0;
    const bool ended = clip->streamEnded.load(std::memory_order_acquire);

    for (int i = startFrame; i < numFrames; i++) {
        float sL = 0.0f, sR = 0.0f;
        if (rf < wf) { // else underrun → silence, hold the cursor
            int slot = static_cast<int>(rf % cap);
            if (ch == 2) { sL = clip->samples[slot * 2]; sR = clip->samples[slot * 2 + 1]; }
            else { float s = clip->samples[slot]; sL = s; sR = s; }
            rf++;
        } else if (!ended) {
            underruns++;
        }

        float g = pb->smoothGain.next();
        float clipPan = pb->smoothPan.next();
        float panL, panR;
        panGains(clipPan, panL, panR);

        float outL, outR;
        if (ch == 2) {
            float L = sL * g, R = sR * g;
            outL = L * panL + R * (1.0f - panR);
            outR = R * panR + L * (1.0f - panL);
        } else {
            float s = sL * g;
            outL = s * panL;
            outR = s * panR;
        }

        if (spatialFilterActive)
            pb->spatialFilter.process(outL, outR, headParams);

        targetBuf[i * 2]     += outL;
        targetBuf[i * 2 + 1] += outR;
        if (clipSendBuf) {
            clipSendBuf[i * 2]     += outL * clipSendAmt;
            clipSendBuf[i * 2 + 1] += outR * clipSendAmt;
        }
    }
    pb->playPos.store(rf, std::memory_order_relaxed);
    if (underruns > 0)
        clip->streamUnderrunFrames.fetch_add(static_cast<uint64_t>(underruns),
                                             std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Streaming PCM source — a ring-backed clip + a persistent playback. Reuses the
// full ClipPlayback machinery (gain/pan/bus/sends/spatial); the mixer takes a
// ring-read branch when clip->streaming is set.
// ---------------------------------------------------------------------------
int Engine::createStreamPlayback(int channels, int ringFrames, bool startPlaying,
                                 float gain, bool loop,
                                 std::shared_ptr<AudioClip>& outClip,
                                 std::shared_ptr<ClipPlayback>& outPb)
{
    auto clip = std::make_shared<AudioClip>();
    auto pb   = std::make_shared<ClipPlayback>();

    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    clip->id = nextClipId_++;
    clip->channels = channels;
    clip->streaming = true;
    clip->ringFrames = ringFrames;
    clip->samples.assign(static_cast<size_t>(ringFrames) * channels, 0.0f);
    clip->writeFrames.store(0, std::memory_order_relaxed);
    int clipId = clip->id;

    pb->id = nextPlaybackId_++;
    pb->clipId = clipId;
    pb->gain.store(gain, std::memory_order_relaxed);
    pb->looping.store(loop, std::memory_order_relaxed);
    pb->playing.store(startPlaying, std::memory_order_relaxed);
    pb->active.store(true, std::memory_order_relaxed);
    pb->playPos.store(0, std::memory_order_relaxed); // absolute read-frame cursor
    int id = pb->id;

    outClip = clip;
    outPb   = pb;

    auto newClips = std::make_shared<ClipList>(*clips_.load());
    newClips->push_back(std::move(clip));
    clips_.store(std::move(newClips));

    auto newPB = std::make_shared<PlaybackList>(*playbacks_.load());
    newPB->push_back(std::move(pb));
    playbacks_.store(std::move(newPB));
    return id;
}

int Engine::createStream(int channels, int ringFrames)
{
    if (channels < 1 || channels > 2) return -1;
    if (ringFrames <= 0) ringFrames = sampleRate_ * 2; // ~2 s default

    std::shared_ptr<AudioClip> clip;
    std::shared_ptr<ClipPlayback> pb;
    return createStreamPlayback(channels, ringFrames, /*startPlaying=*/true,
                                1.0f, false, clip, pb);
}

int Engine::pushStreamSamples(int instanceId, const float* samples, int numSamples)
{
    if (!samples || numSamples <= 0) return 0;
    // No write lock: this is the single producer. We only read the (stable)
    // clip pointer + write ring slots, then release-publish writeFrames.
    ClipPlayback* pb = findPlayback(instanceId);
    if (!pb) return 0;
    AudioClip* clip = findClip(pb->clipId);
    if (!clip || !clip->streaming || clip->ringFrames <= 0) return 0;

    int numFrames = numSamples / clip->channels;
    if (numFrames <= 0) return 0;
    return pushStreamFrames(*clip, samples, numFrames);
}

void Engine::closeStream(int instanceId)
{
    // Disk streams: stop + join the decode worker before dropping the ring.
    stopFileStream(instanceId);

    ClipPlayback* pb = findPlayback(instanceId);
    int clipId = pb ? pb->clipId : -1;
    stopPlayback(instanceId);
    if (clipId >= 0) deleteClip(clipId);
}

// ---------------------------------------------------------------------------
// Disk-streamed file playback — decode on a cold worker thread into the same
// SPSC ring a live PCM stream uses; the audio thread only consumes.
// ---------------------------------------------------------------------------

int Engine::createStreamFromFile(const char* path, std::string* outError)
{
    return createStreamFromFile(path, FileStreamOptions{}, outError);
}

int Engine::createStreamFromFile(const char* path, const FileStreamOptions& opts,
                                 std::string* outError)
{
    auto fail = [&](const std::string& msg) {
        if (outError) *outError = msg;
        log(LogLevel::Error, "createStreamFromFile: %s: %s",
            path ? path : "(null)", msg.c_str());
        return -1;
    };

    auto decoder = std::make_unique<AudioFileStream>();
    if (!decoder->open(path)) {
        return fail(decoder->error().empty() ? "cannot open or decode file"
                                             : decoder->error());
    }
    if (decoder->channels() < 1 || decoder->channels() > 2) {
        return fail("only mono and stereo files can be streamed (file has "
                    + std::to_string(decoder->channels()) + " channels)");
    }
    if (decoder->sampleRate() <= 0) {
        return fail("file reports an invalid sample rate");
    }

    int ringFrames = opts.ringFrames > 0 ? opts.ringFrames : sampleRate_ * 2;   // ~2 s
    int prebuffer  = opts.prebufferFrames > 0 ? opts.prebufferFrames : sampleRate_ / 2; // ~500 ms
    prebuffer = std::min(prebuffer, ringFrames / 2);

    std::shared_ptr<AudioClip> clip;
    std::shared_ptr<ClipPlayback> pb;
    // startPlaying=false: the worker releases playback once the prebuffer is
    // decoded, so the mixer never counts pre-start silence as underrun.
    int id = createStreamPlayback(decoder->channels(), ringFrames,
                                  /*startPlaying=*/false, opts.gain, opts.loop,
                                  clip, pb);

    auto runner = std::make_unique<FileStreamRunner>(
        std::move(clip), std::move(pb), std::move(decoder), sampleRate_, prebuffer);
    runner->start();

    {
        std::lock_guard<std::mutex> lock(fileStreamsMutex_);
        fileStreams_.push_back(std::move(runner));
    }
    return id;
}

StreamStats Engine::getStreamStats(int instanceId) const
{
    StreamStats s;
    ClipPlayback* pb = findPlayback(instanceId);
    if (!pb) return s;
    AudioClip* clip = findClip(pb->clipId);
    if (!clip || !clip->streaming) return s;

    s.valid = true;
    s.decodedFrames = clip->writeFrames.load(std::memory_order_acquire);
    s.playedFrames = pb->playPos.load(std::memory_order_relaxed);
    s.bufferedFrames = s.decodedFrames > s.playedFrames
                           ? s.decodedFrames - s.playedFrames : 0;
    s.underrunFrames = clip->streamUnderrunFrames.load(std::memory_order_relaxed);
    s.finished = clip->streamEnded.load(std::memory_order_acquire)
                 && s.bufferedFrames == 0;
    return s;
}

void Engine::stopFileStream(int instanceId)
{
    std::unique_ptr<FileStreamRunner> runner;
    {
        std::lock_guard<std::mutex> lock(fileStreamsMutex_);
        for (auto it = fileStreams_.begin(); it != fileStreams_.end(); ++it) {
            if ((*it)->playbackId() == instanceId) {
                runner = std::move(*it);
                fileStreams_.erase(it);
                break;
            }
        }
    }
    // Join outside the lock — the worker never touches fileStreams_, but a
    // slow decode step shouldn't stall unrelated stream creation.
    if (runner) {
        runner->requestStop();
        runner->join();
    }
}

void Engine::stopAllFileStreams()
{
    std::vector<std::unique_ptr<FileStreamRunner>> runners;
    {
        std::lock_guard<std::mutex> lock(fileStreamsMutex_);
        runners.swap(fileStreams_);
    }
    for (auto& r : runners) r->requestStop();  // signal all first
    for (auto& r : runners) r->join();
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

void Engine::seekPlayback(int instanceId, double seconds)
{
    if (seconds < 0.0) seconds = 0.0;

    // Disk streams: hand the seek to the decode worker (codec seek + ring
    // flush fence). Control-plane mutex only — the audio thread never touches
    // fileStreams_.
    {
        std::lock_guard<std::mutex> lock(fileStreamsMutex_);
        for (auto& r : fileStreams_) {
            if (r->playbackId() == instanceId) {
                r->requestSeek(seconds);
                return;
            }
        }
    }

    ClipPlayback* pb = findPlayback(instanceId);
    if (!pb) return;
    AudioClip* clip = findClip(pb->clipId);
    if (!clip) return;
    if (clip->streaming) return;  // live PCM stream — nothing to seek into

    int rs = pb->regionStart.load(std::memory_order_relaxed);
    int re = pb->regionEnd.load(std::memory_order_relaxed);
    int end = re > 0 ? re : clip->numFrames();
    int len = end - rs;
    if (len <= 0) return;

    int64_t frame = static_cast<int64_t>(seconds * sampleRate_ + 0.5);
    if (frame >= len) frame = len - 1;
    // playPos is a 16.16 fixed-point cursor relative to the region start.
    // Racing the audio thread's block-end store is benign (same contract as
    // setPlaybackPlaying / setPlaybackRegion): worst case the seek lands one
    // mix block late.
    pb->playPos.store(static_cast<uint64_t>(frame) << 16, std::memory_order_relaxed);
}

double Engine::getPlaybackPositionSeconds(int instanceId) const
{
    auto* pb = findPlayback(instanceId);
    if (!pb) return 0.0;
    auto* clip = findClip(pb->clipId);
    if (!clip) return 0.0;

    uint64_t pos = pb->playPos.load(std::memory_order_relaxed);

    if (clip->streaming) {
        // playPos counts consumed engine-rate frames; the seek fence + base
        // rebase it into file time (both zero for never-seeked streams and
        // live PCM streams).
        uint64_t flush = clip->streamFlushFrames.load(std::memory_order_acquire);
        int64_t base = clip->streamPosBaseFrames.load(std::memory_order_relaxed);
        uint64_t sinceFlush = pos > flush ? pos - flush : 0;
        double s = (static_cast<double>(base) + static_cast<double>(sinceFlush))
                   / static_cast<double>(sampleRate_);
        return s > 0.0 ? s : 0.0;
    }

    int rs = pb->regionStart.load(std::memory_order_relaxed);
    int re = pb->regionEnd.load(std::memory_order_relaxed);
    int end = re > 0 ? re : clip->numFrames();
    int len = end - rs;
    if (len <= 0) return 0.0;
    int64_t intPos = static_cast<int64_t>(pos >> 16);
    if (pb->looping.load(std::memory_order_relaxed)) intPos %= len;
    else if (intPos > len) intPos = len;
    return static_cast<double>(intPos) / static_cast<double>(sampleRate_);
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
                bus.filters[f].snapToTarget();
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

void Engine::processBusDistortion(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.distortionParams.version.load(std::memory_order_acquire);
    if (ver != bus.distortionVersion) {
        bus.distortionVersion = ver;
        bus.distortion.enabled = bus.distortionParams.enabled.load(std::memory_order_relaxed);
        bus.distortion.mode = static_cast<DistortionMode>(bus.distortionParams.mode.load(std::memory_order_relaxed));
        bus.distortion.drive = bus.distortionParams.drive.load(std::memory_order_relaxed);
        bus.distortion.mix = bus.distortionParams.mix.load(std::memory_order_relaxed);
        bus.distortion.outputGain = bus.distortionParams.outputGain.load(std::memory_order_relaxed);
        bus.distortion.crushBits = bus.distortionParams.crushBits.load(std::memory_order_relaxed);
        bus.distortion.crushRate = bus.distortionParams.crushRate.load(std::memory_order_relaxed);
    }
    if (bus.distortion.enabled) {
        bus.distortion.processStereo(buf, numFrames);
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
            case EffectSlot::Distortion: processBusDistortion(bus, buf, numFrames); break;
            default: break;
        }
    }

    updateBusMeters(bus, numFrames);
}

void Engine::mixBusIntoParent(Bus& child, Bus& parent, int numFrames)
{
    if (child.muted.load(std::memory_order_relaxed)) return;

    child.smoothGain.set(child.gain.load(std::memory_order_relaxed));
    child.smoothPan.set(child.pan.load(std::memory_order_relaxed));

    float* src = child.buffer.data();
    float* dst = parent.buffer.data();

    for (int i = 0; i < numFrames; i++) {
        float g = child.smoothGain.next();
        float panVal = child.smoothPan.next();
        float panL, panR;
        panGains(panVal, panL, panR);

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
    int totalFloats = additional_amount / static_cast<int>(sizeof(float));
    int totalFrames = totalFloats / 2;
    if (totalFrames <= 0) return;

    // Master pause: feed silence without touching the graph or advancing
    // samplesGenerated_ — voices, clips, and scheduled events freeze in
    // place and resume exactly where they stopped. outputScratch_ is owned
    // by this (audio) thread, so reusing it here is race-free.
    if (engine->masterPaused_.load(std::memory_order_relaxed)) {
        float* buffer = engine->outputScratch_.data();
        std::memset(buffer, 0,
                    std::min(totalFrames, MAX_SCRATCH_FRAMES) * 2 * sizeof(float));
        int remaining = totalFrames;
        while (remaining > 0) {
            int chunk = std::min(remaining, MAX_SCRATCH_FRAMES);
            SDL_PutAudioStreamData(stream, buffer,
                                   chunk * 2 * static_cast<int>(sizeof(float)));
            remaining -= chunk;
        }
        return;
    }

    // Reader scope: brackets every RCU load on the audio thread so writers
    // can safely retire old snapshots without freeing them out from under us.
    RcuDomain::ReadScope rcuScope(engine->rcu_);

    // Chunk to MAX_SCRATCH_FRAMES so we never need to allocate on the audio
    // thread. Bus buffers are sized for MAX_SCRATCH_FRAMES, so this is also
    // a hard ceiling on what the pipeline can process per pass.
    int remaining = totalFrames;
    while (remaining > 0) {
        int chunk = std::min(remaining, MAX_SCRATCH_FRAMES);
        engine->processOutputChunk(stream, chunk);
        remaining -= chunk;
    }
}

void Engine::processOutputChunk(SDL_AudioStream* stream, int numFrames)
{
    auto* engine = this;
    int numFloats = numFrames * 2;
    float* buffer = outputScratch_.data();

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
        // This block spans absolute samples [blockStart, blockStart+numFrames).
        // generateSamples() already advanced samplesGenerated_ by numFrames above,
        // so the counter now holds the block's END sample.
        uint64_t blockEnd = engine->samplesGenerated_.load(std::memory_order_relaxed);
        uint64_t blockStart = blockEnd >= static_cast<uint64_t>(numFrames)
                                  ? blockEnd - static_cast<uint64_t>(numFrames) : 0;
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
            float rate = pb->rate.load(std::memory_order_relaxed);
            bool looping = pb->looping.load(std::memory_order_relaxed);
            int ch = clip->channels;

            // Set smoother targets for gain and pan
            float targetGain2 = pb->gain.load(std::memory_order_relaxed);
            float targetPan2 = pb->pan.load(std::memory_order_relaxed);

            // Spatial override for clip playback
            HeadParams headParams2;
            bool spatialFilterActive2 = false;
            if (pb->spatial.spatialEnabled.load(std::memory_order_relaxed)) {
                auto sr = computeSpatial(engine->listener_, pb->spatial);
                targetGain2 *= sr.gain;
                targetPan2 = 0.0f; // center — head model does L/R
                headParams2 = computeHeadParams(sr, engine->headModel_, engine->sampleRate_);
                spatialFilterActive2 = true;
            }

            pb->smoothGain.set(targetGain2);
            pb->smoothPan.set(targetPan2);

            // Find clip send bus buffer (if configured)
            int clipSendId = pb->sendBusId.load(std::memory_order_relaxed);
            float clipSendAmt = pb->sendAmount.load(std::memory_order_relaxed);
            float* clipSendBuf = nullptr;
            if (clipSendId >= 0 && clipSendAmt > 0.0f) {
                for (auto& bus : *currentBuses) {
                    if (bus->id == clipSendId) { clipSendBuf = bus->buffer.data(); break; }
                }
            }

            // Streaming sources read from a ring instead of a fixed clip.
            if (clip->streaming) {
                engine->mixStreamPlayback(pb.get(), clip, targetBuf, clipSendBuf, clipSendAmt,
                                          spatialFilterActive2, headParams2, numFrames, 0);
                continue;
            }

            constexpr int FRAC_BITS = 16;
            constexpr uint64_t FRAC_MASK = (1ULL << FRAC_BITS) - 1;
            uint64_t increment = static_cast<uint64_t>(rate * (1 << FRAC_BITS) + 0.5f);

            // Sample-accurate scheduled start: stay silent until the audio clock
            // reaches startSample, then begin mid-block at the exact frame. 0 (or
            // any past sample) starts immediately. Once started, later blocks have
            // startSample <= blockStart, so startFrame is 0 and playPos continues.
            int startFrame = 0;
            uint64_t startS = pb->startSample.load(std::memory_order_relaxed);
            if (startS > blockStart) {
                uint64_t off = startS - blockStart;
                if (off >= static_cast<uint64_t>(numFrames)) continue;  // starts in a later block
                startFrame = static_cast<int>(off);
            }

            for (int i = startFrame; i < numFrames; i++) {
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

                float g = pb->smoothGain.next();
                float clipPan = pb->smoothPan.next();
                float panL, panR;
                panGains(clipPan, panL, panR);

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

                if (spatialFilterActive2)
                    pb->spatialFilter.process(outL, outR, headParams2);

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
        uint64_t rp = engine->micPlaybackReadPos_.load(std::memory_order_relaxed);
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
        engine->micPlaybackReadPos_.store(rp + toRead, std::memory_order_relaxed);
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

        // Solo gate (see renderInternal): silenced buses skip the parent mix
        // and aux send but keep their effect tails running.
        bool audible = engine->busAudibleUnderSolo(*currentBuses, *bus);

        // Find parent and mix into it
        int parentId = bus->parentId.load(std::memory_order_relaxed);
        for (auto& parent : *currentBuses) {
            if (parent->id == parentId) {
                if (audible) engine->mixBusIntoParent(*bus, *parent, numFrames);
                break;
            }
        }

        // Bus-to-bus aux send (post-effects, post-fader)
        int busSendId = bus->sendBusId.load(std::memory_order_relaxed);
        float busSendAmt = bus->sendAmount.load(std::memory_order_relaxed);
        if (audible && busSendId >= 0 && busSendAmt > 0.0f) {
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

    // Copy master bus to output buffer, apply master gain (smoothed) + limiter
    if (masterBus) {
        engine->smoothMasterGain_.set(engine->masterGain_.load(std::memory_order_relaxed));
        for (int i = 0; i < numFrames; i++) {
            float mg = engine->smoothMasterGain_.next();
            buffer[i * 2]     = masterBus->buffer[i * 2]     * mg;
            buffer[i * 2 + 1] = masterBus->buffer[i * 2 + 1] * mg;
        }
        engine->masterLimiter_.process(buffer, static_cast<size_t>(numFrames));
    } else {
        std::memset(buffer, 0, numFloats * sizeof(float));
    }

    // Mix mic monitor (direct-to-output, only when not routed through a bus)
    if (micBusId < 0 && !engine->micMuted_.load(std::memory_order_relaxed)) {
        float micGain = engine->micMonitorGain_.load(std::memory_order_relaxed);
        uint64_t wp = engine->micPlaybackWritePos_.load(std::memory_order_acquire);
        uint64_t rp = engine->micPlaybackReadPos_.load(std::memory_order_relaxed);
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
        engine->micPlaybackReadPos_.store(rp + toRead, std::memory_order_relaxed);
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
                // Snap smoothers to initial values (no fade-in from 0)
                voice.smoothGain.init(sampleRate_);
                voice.smoothGain.snap(VOICE_AMPLITUDE * voice.gain.load(std::memory_order_relaxed));
                voice.smoothPan.init(sampleRate_);
                voice.smoothPan.snap(voice.pan.load(std::memory_order_relaxed));
                voice.smoothFreq.init(sampleRate_);
                voice.smoothFreq.snap(voice.frequency.load(std::memory_order_relaxed));
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

        voice.smoothFreq.set(voice.frequency.load(std::memory_order_relaxed));
        voice.smoothGain.set(VOICE_AMPLITUDE * voice.gain.load(std::memory_order_relaxed));
        voice.smoothPan.set(voice.pan.load(std::memory_order_relaxed));
        float pitchBendSemitones = voice.pitchBend.load(std::memory_order_relaxed);
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

        // Pre-compute spatial result once per block (positions don't change within a callback)
        float voiceSpatialGain = 1.0f;
        float voiceSpatialPan = 0.0f;
        HeadParams voiceHeadParams;
        bool voiceSpatialActive = voice.spatial.spatialEnabled.load(std::memory_order_relaxed);
        if (voiceSpatialActive) {
            auto sr = computeSpatial(listener_, voice.spatial);
            voiceSpatialGain = sr.gain;
            voiceSpatialPan = sr.pan;
            voiceHeadParams = computeHeadParams(sr, headModel_, sampleRate_);
        }

        // Filter parameter pickup (block-rate). Coefficients are recomputed
        // once per block when modulation is non-unity, then smoothly
        // interpolated across the block by stepSmoothing(). Per-sample
        // recompute (sin/cos/pow) is too expensive at polyphony.
        bool filterStateChanged = false;
        float filterBaseCutoff = 1000.0f;
        float filterBaseQ = 1.0f;
        {
            uint32_t fv = voice.filterVersion.load(std::memory_order_acquire);
            if (fv != voice.filterVersionSeen) {
                voice.filterVersionSeen = fv;
                bool wasEnabled = voice.filter.enabled;
                voice.filter.enabled = voice.filterEnabled.load(std::memory_order_relaxed);
                voice.filter.type = static_cast<BiquadFilter::Type>(
                    voice.filterType.load(std::memory_order_relaxed));
                filterStateChanged = true;
                if (voice.filter.enabled && !wasEnabled) {
                    voice.filter.reset();
                    voice.filter.primed = false; // snap on first compute
                }
            }
            filterBaseCutoff = voice.filterFrequency.load(std::memory_order_relaxed);
            filterBaseQ = voice.filterQ.load(std::memory_order_relaxed);
        }

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
                    if (susLevel < 0.0001f) {
                        voice.envLevel = 0.0f;
                        voice.envStage = EnvStage::Done;
                        voice.active = false;
                    }
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

            // Step smoothers per sample
            float baseFreq = voice.smoothFreq.next();
            float gain = voice.smoothGain.next();
            float basePan = voice.smoothPan.next();

            // Apply pitch bend + mod pitch to frequency
            float totalPitchSemitones = pitchBendSemitones + mod.pitch;
            float freq = baseFreq * std::exp2(totalPitchSemitones / 12.0f);

            // Apply mod gain and pan
            float finalGain = gain * voice.envLevel * mod.gain;
            float finalPan = std::clamp(basePan + mod.pan, -1.0f, 1.0f);

            // Spatial override: if enabled, apply distance attenuation but skip panGains —
            // the head model handles all L/R differentiation.
            if (voiceSpatialActive) {
                finalGain *= voiceSpatialGain;
                finalPan = 0.0f; // center — head model does L/R
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

            // Per-voice filter (modulated by mod matrix) — runs on mono sum.
            // Coefficients are recomputed at block start (i == 0) using mod
            // values from this sample, then smoothly interpolated across the
            // block. This keeps the expensive sin/cos out of the inner loop.
            if (voice.filter.enabled) {
                if (i == 0 || filterStateChanged) {
                    voice.filter.frequency = std::clamp(filterBaseCutoff * mod.filterFreq, 20.0f, 20000.0f);
                    voice.filter.Q = std::clamp(filterBaseQ * mod.filterQ, 0.1f, 30.0f);
                    voice.filter.computeCoefficients(sampleRate_);
                    voice.filter.prepareSmoothing(numFrames);
                    filterStateChanged = false;
                }
                float filteredMono = voice.filter.process(monoSum, 0);
                voice.filter.stepSmoothing();
                float ratio = (monoSum != 0.0f) ? filteredMono / monoSum : 0.0f;
                sumL *= ratio;
                sumR *= ratio;
            }

            float outL = sumL * finalGain;
            float outR = sumR * finalGain;

            // Directional filter: front/back + elevation cues
            if (voiceSpatialActive)
                voice.spatialFilter.process(outL, outR, voiceHeadParams);

            buf[i * 2]     += outL;
            buf[i * 2 + 1] += outR;

            // Aux send (base amount + mod matrix delaySend)
            if (sendBuf) {
                float sendLevel = std::clamp(baseSendAmt + mod.delaySend, 0.0f, 1.0f);
                float sendGain = gain * voice.envLevel * sendLevel;
                sendBuf[i * 2]     += sumL * sendGain;
                sendBuf[i * 2 + 1] += sumR * sendGain;
            }
        }
    }

    // Flag finished voices for non-realtime cleanup. The actual list rebuild
    // (which involves heap alloc/dealloc) happens in Engine::update() on a
    // non-audio thread. Persistent voices (VoiceAllocator-managed) are
    // ignored here — they stay in the list for reuse.
    if (!voiceCleanupNeeded_.load(std::memory_order_relaxed)) {
        for (auto& v : *currentVoices) {
            if (v->started && (v->envStage == EnvStage::Done || !v->active)
                && !v->persistent.load(std::memory_order_relaxed)) {
                voiceCleanupNeeded_.store(true, std::memory_order_release);
                break;
            }
        }
    }

    samplesGenerated_.fetch_add(static_cast<uint64_t>(numFrames), std::memory_order_relaxed);
}

void Engine::reapFinishedVoicesLocked()
{
    // Caller must hold voiceWriteMutex_.
    auto current = voices_.load();
    bool needRebuild = false;
    for (auto& v : *current) {
        if (v->started && (v->envStage == EnvStage::Done || !v->active)
            && !v->persistent.load(std::memory_order_relaxed)) {
            needRebuild = true;
            break;
        }
    }
    if (!needRebuild) return;

    auto newList = std::make_shared<VoiceList>();
    newList->reserve(current->size());
    for (auto& v : *current) {
        bool finished = v->started && (v->envStage == EnvStage::Done || !v->active);
        bool persistent = v->persistent.load(std::memory_order_relaxed);
        if (!finished || persistent) newList->push_back(v);
    }
    voices_.store(std::move(newList));
}

void Engine::update()
{
    if (voiceCleanupNeeded_.exchange(false, std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(voiceWriteMutex_);
        reapFinishedVoicesLocked();
    }
    rcu_.reclaim();
}

// ---------------------------------------------------------------------------
// Audio file I/O convenience
// ---------------------------------------------------------------------------

int Engine::createClipFromFile(const char* path)
{
    return createClipFromFileEx(path, nullptr);
}

int Engine::createClipFromFileEx(const char* path, std::string* outError)
{
    auto fail = [&](const std::string& msg) {
        if (outError) *outError = msg;
        log(LogLevel::Error, "createClipFromFile: %s: %s",
            path ? path : "(null)", msg.c_str());
        return -1;
    };

    AudioFileData data = loadAudioFile(path);
    if (!data.valid()) {
        // Surface a known decode error (e.g. corrupt Vorbis stream) — the
        // int return can only say "failed", which reads as a bad path.
        return fail(!data.error.empty()
                        ? data.error
                        : "cannot open or decode file (supported formats: WAV, "
                          "FLAC, MP3, Ogg Vorbis)");
    }

    // Reject clips that exceed the decoded size limit
    if (maxClipDecodedBytes_ > 0 &&
        data.samples.size() * sizeof(float) > maxClipDecodedBytes_) {
        return fail("decoded audio ("
                    + std::to_string(data.samples.size() * sizeof(float) / (1024 * 1024))
                    + " MB) exceeds the clip size limit ("
                    + std::to_string(maxClipDecodedBytes_ / (1024 * 1024))
                    + " MB); use createStreamFromFile to disk-stream large files");
    }

    // Resample to engine sample rate if needed
    if (data.sampleRate != sampleRate_) {
        auto resampled = resample(data.samples.data(), data.numFrames,
                                  data.channels, data.sampleRate, sampleRate_);
        if (resampled.empty()) return fail("resampling failed");
        return createClip(resampled.data(),
                          static_cast<int>(resampled.size()),
                          data.channels);
    }

    return createClip(data.samples.data(),
                      static_cast<int>(data.samples.size()),
                      data.channels);
}

std::future<int> Engine::createClipFromFileAsync(const char* path)
{
    std::string pathStr(path ? path : "");
    return std::async(std::launch::async, [this, pathStr]() -> int {
        // Decode + resample run on this background thread; createClip locks
        // the control-plane media mutex, which is safe off the main thread.
        return createClipFromFileEx(pathStr.c_str(), nullptr);
    });
}

bool Engine::exportRecordingToWav(const char* path)
{
    auto buf = getRecordBuffer();
    if (buf.empty()) return false;
    // Record buffer is mono (single channel)
    int numFrames = static_cast<int>(buf.size());
    return saveWav(path, buf.data(), numFrames, 1, sampleRate_);
}

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
