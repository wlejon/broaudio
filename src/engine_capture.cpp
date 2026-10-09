// Microphone capture and the device recording callback, multi-consumer mic
// taps (resample + AGC + chunking), output recording, and spectrum analysis.

#include "broaudio/engine.h"
#include "broaudio/log.h"
#include "broaudio/dsp/fft.h"
#include "device/stream_resampler.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace broaudio {

// ---------------------------------------------------------------------------
// Microphone capture
// ---------------------------------------------------------------------------

bool Engine::startMicCapture()
{
    if (micCapturing_) return true;
    if (!initialized_) return false;

    // A headless engine has no device backend, and the mic stays closed: a
    // headless run never claims the machine's sound hardware (offline mic
    // input goes through injectMicSamples).
    if (!backend_) return false;

    AudioStreamConfig sc;
    sc.direction = AudioDirection::Capture;
    sc.deviceId = deviceConfig_.inputDevice;
    sc.sampleRate = sampleRate_;
    sc.channels = 1;
    sc.periodFrames = deviceConfig_.periodFrames;
    sc.appId = deviceConfig_.appId;
    sc.appName = deviceConfig_.appName;
    sc.streamName = "Microphone";
    std::string error;
    micStream_ = backend_->openStream(sc, &Engine::micCallback, this, &error);
    if (!micStream_) {
        log(LogLevel::Error, "broaudio: Failed to open mic device (%s): %s",
            backend_->name(), error.c_str());
        return false;
    }
    if (!micStream_->start()) {
        log(LogLevel::Error, "broaudio: Failed to start mic device (%s)", backend_->name());
        micStream_.reset();
        return false;
    }
    micCapturing_ = true;
    return true;
}

void Engine::stopMicCapture()
{
    if (!micCapturing_) return;
    if (micStream_) {
        micStream_->stop();
        micStream_.reset();
    }
    micCapturing_ = false;
}

void Engine::micCallback(void* userdata, float* in, int numFrames)
{
    static_cast<Engine*>(userdata)->processMicSamples(in, numFrames);
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
    delete resampler;
    resampler = nullptr;
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
        tap->resampler = StreamResampler::create(1, engineRate, cfg.targetRate).release();
        if (!tap->resampler) {
            log(LogLevel::Error, "broaudio: addMicTap: could not create a %d -> %d Hz resampler",
                engineRate, cfg.targetRate);
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
    // the MicTap destructor runs and frees the resampler.
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
        tap.resampler->put(samples, numSamples);
        const int availSamples = tap.resampler->available();
        if (availSamples <= 0) return;
        if (static_cast<int>(tap.scratch.size()) < availSamples) {
            tap.scratch.resize(static_cast<std::size_t>(availSamples));
        }
        const int got = tap.resampler->get(tap.scratch.data(), availSamples);
        if (got <= 0) return;
        feed = tap.scratch.data();
        feedN = got;
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

void Engine::startRecording(int channels, double maxSeconds)
{
    channels = channels >= 2 ? 2 : 1;
    if (!(maxSeconds > 0.0)) maxSeconds = 60.0;
    double wantFrames = std::ceil(maxSeconds * static_cast<double>(sampleRate_));
    double capFrames = static_cast<double>(MAX_RECORD_SAMPLES / static_cast<size_t>(channels));
    uint64_t frames = static_cast<uint64_t>(std::max(1.0, std::min(wantFrames, capFrames)));

    recording_.store(false, std::memory_order_release);
    auto tap = recordTap_.load();
    if (!tap || tap->channels != channels || tap->frames != frames) {
        auto fresh = std::make_shared<RecordTap>();
        fresh->channels = channels;
        fresh->frames = frames;
        fresh->ring.assign(static_cast<size_t>(frames) * static_cast<size_t>(channels), 0.0f);
        std::lock_guard<std::mutex> lock(mediaWriteMutex_);
        recordTap_.store(std::move(fresh));
    }
    recordStartPos_.store(recordWritePos_.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    recording_.store(true, std::memory_order_release);
}

void Engine::tapRecord(const float* stereo, int numFrames)
{
    if (!recording_.load(std::memory_order_relaxed)) return;
    auto tap = recordTap_.load();
    if (!tap || tap->frames == 0) return;
    uint64_t wp = recordWritePos_.load(std::memory_order_relaxed);
    float* ring = tap->ring.data();
    const uint64_t cap = tap->frames;
    if (tap->channels == 2) {
        for (int i = 0; i < numFrames; i++) {
            size_t at = static_cast<size_t>((wp + i) % cap) * 2;
            ring[at] = stereo[i * 2];
            ring[at + 1] = stereo[i * 2 + 1];
        }
    } else {
        for (int i = 0; i < numFrames; i++) {
            ring[static_cast<size_t>((wp + i) % cap)] = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
        }
    }
    recordWritePos_.store(wp + numFrames, std::memory_order_release);
}

void Engine::stopRecording()
{
    recording_.store(false, std::memory_order_release);

    auto tap = recordTap_.load();
    uint64_t endPos = recordWritePos_.load(std::memory_order_acquire);
    uint64_t startPos = recordStartPos_.load(std::memory_order_relaxed);
    if (!tap || endPos <= startPos) {
        recordOutput_.clear();
        return;
    }
    uint64_t count = endPos - startPos;
    if (count > tap->frames) {
        startPos = endPos - tap->frames;
        count = tap->frames;
    }
    const int ch = tap->channels;
    recordOutputChannels_ = ch;
    recordOutput_.resize(static_cast<size_t>(count) * static_cast<size_t>(ch));
    for (uint64_t i = 0; i < count; i++) {
        size_t from = static_cast<size_t>((startPos + i) % tap->frames) * static_cast<size_t>(ch);
        for (int c = 0; c < ch; c++) recordOutput_[static_cast<size_t>(i) * ch + c] = tap->ring[from + c];
    }
}

// ---------------------------------------------------------------------------
// Spectrum analysis
// ---------------------------------------------------------------------------

std::vector<float> Engine::getSpectrum(int numBins) const
{
    if (numBins <= 0 || numBins > 8192) return {};

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
    std::vector<float> result(numBins, 0.0f);
    float invN = 1.0f / static_cast<float>(n);
    for (int i = 0; i < outBins; i++) {
        float re = real[i] * invN;
        float im = imag[i] * invN;
        result[i] = std::sqrt(re * re + im * im) * 2.0f;
    }

    return result;
}

} // namespace broaudio
