// Engine lifecycle (init/headless/renderBlock/shutdown) and the lock-free
// audio thread: the SDL output callback, per-chunk bus-graph mixdown, voice
// synthesis (generateSamples) and finished-voice reaping.

#include "broaudio/engine.h"
#include "broaudio/clip/file_stream.h"
#include "broaudio/log.h"
#include "broaudio/synth/oscillator.h"
#include "broaudio/synth/wavetable.h"
#include "broaudio/dsp/limiter.h"
#include "engine_internal.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace broaudio {

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
        master->jitPipeline.setDomain(rcu_);
        master->convolver.setDomain(rcu_);
        syncBusJitTopology(*master);

        auto list = std::make_shared<BusList>();
        list->push_back(std::move(master));
        buses_.store(std::move(list));
    }

    // Pre-allocate scratch buffers
    outputScratch_.resize(MAX_SCRATCH_FRAMES * 2, 0.0f);
    micScratch_.resize(MAX_SCRATCH_FRAMES, 0.0f);
    initDistanceState(MAX_SCRATCH_FRAMES);

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
        master->convolver.setDomain(rcu_);

        auto list = std::make_shared<BusList>();
        list->push_back(std::move(master));
        buses_.store(std::move(list));
    }

    outputScratch_.resize(MAX_SCRATCH_FRAMES * 2, 0.0f);
    micScratch_.resize(MAX_SCRATCH_FRAMES, 0.0f);
    initDistanceState(MAX_SCRATCH_FRAMES);

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
    mixPlaybacks(numFrames, *currentBuses);

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
                    mixBusSend(*bus, *sendTarget, busSendAmt, numFrames);
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

    tapRecord(buffer, numFrames);

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
    jitCompiler_.shutdown();
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
    engine->mixPlaybacks(numFrames, *currentBuses);

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

    Bus* masterBus = nullptr;
    for (auto& bus : *currentBuses) {
        if (bus->id == MASTER_BUS_ID) { masterBus = bus.get(); break; }
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
                    mixBusSend(*bus, *sendTarget, busSendAmt, numFrames);
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
    engine->tapRecord(buffer, numFrames);

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
                        v->markStart(ev.when);
                    } else {
                        v->markRelease(ev.when);
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

        if (voice.triggerStart.exchange(false, std::memory_order_acquire)) {
            // A pending release older than this start (a rapid noteOff +
            // noteOn) must not put the new note straight into Release; one
            // issued after it (start + stop in the same tick) still applies,
            // below. The stamps order them (Voice::markStart / markRelease).
            voice.appliedStartSeq = voice.startSeq.load(std::memory_order_relaxed);
            voice.pendingReleaseAt = std::numeric_limits<double>::infinity();
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

        if (voice.triggerRelease.exchange(false, std::memory_order_acquire)) {
            const uint32_t rel = voice.releaseSeq.load(std::memory_order_relaxed);
            const bool predatesStart =
                static_cast<int32_t>(rel - voice.appliedStartSeq) < 0;
            // The release lands at its own time inside the block (below),
            // so a noteOff scheduled later than a noteOn in the same block
            // still lets the note sound until then.
            if (!predatesStart) {
                voice.pendingReleaseAt = std::min(voice.pendingReleaseAt,
                                                  voice.releaseTime.load(std::memory_order_relaxed));
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
            voiceHeadParams = computeHeadParams(sr, headModel_, sampleRate_,
                                                voice.spatial.occlusion.load(std::memory_order_relaxed));
            // Doppler folds into pitch: 12·log2(ratio) semitones on top of
            // the voice's pitch bend (block-rate, like the rest of spatial).
            float dop = computeDopplerRatio(
                listener_, voice.spatial,
                dopplerFactor_.load(std::memory_order_relaxed),
                speedOfSound_.load(std::memory_order_relaxed)
                    / metresPerUnit_.load(std::memory_order_relaxed));
            voice.spatial.lastDopplerRatio.store(dop, std::memory_order_relaxed);
            if (dop != 1.0f)
                pitchBendSemitones += 12.0f * std::log2(dop);
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

            if (t >= voice.pendingReleaseAt) {
                voice.pendingReleaseAt = std::numeric_limits<double>::infinity();
                if (voice.envStage == EnvStage::Attack || voice.envStage == EnvStage::Decay
                    || voice.envStage == EnvStage::Sustain) {
                    voice.envStage = EnvStage::Release;
                }
            }

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

} // namespace broaudio
