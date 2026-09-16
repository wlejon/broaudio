#include "api.h"
#include "host_audio_internal.h"
#include "object_builder.h"

#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace broaudio::api {

namespace {

constexpr int kMicRing = 4096;

struct MicState {
    std::atomic<int> peakRingX10000[kMicRing];
    std::atomic<int> rmsRingX10000[kMicRing];
    std::atomic<uint64_t> writeCount{0};
    std::atomic<uint64_t> dropped{0};

    bool wantSamples = false;
    std::vector<float> sampleRing;

    uint64_t lastFired = 0;
    int chunkFrames = 0;
    broaudio::MicTapId tapId = broaudio::kInvalidMicTapId;
    bool active = false;

    bronze::embed::Persistent onChunk;
};

static MicState g_mic;

void shutdownActiveMic() {
    if (!g_mic.active) return;
    auto* e = getAudioEngine();
    if (e && g_mic.tapId != broaudio::kInvalidMicTapId) {
        e->removeMicTap(g_mic.tapId);
    }
    g_mic.tapId = broaudio::kInvalidMicTapId;
    g_mic.onChunk.set(ev::undefined());
    g_mic.writeCount.store(0, std::memory_order_relaxed);
    g_mic.dropped.store(0, std::memory_order_relaxed);
    g_mic.lastFired = 0;
    g_mic.chunkFrames = 0;
    g_mic.wantSamples = false;
    g_mic.sampleRing.clear();
    g_mic.sampleRing.shrink_to_fit();
    g_mic.active = false;
}

} // namespace

void drainMicChunks() {
    if (!g_mic.active) return;
    uint64_t w = g_mic.writeCount.load(std::memory_order_acquire);
    if (w == g_mic.lastFired) return;
    if (ev::isUndefined(g_mic.onChunk.get()) || !ev::isFunction(g_mic.onChunk.get())) {
        g_mic.lastFired = w;
        return;
    }

    uint64_t start = g_mic.lastFired;
    if (w - start > static_cast<uint64_t>(kMicRing)) {
        g_mic.dropped.fetch_add(w - start - kMicRing, std::memory_order_relaxed);
        start = w - kMicRing;
    }
    for (uint64_t i = start; i < w; ++i) {
        int slot = static_cast<int>(i % kMicRing);
        int pk = g_mic.peakRingX10000[slot].load(std::memory_order_relaxed);
        int rms = g_mic.rmsRingX10000[slot].load(std::memory_order_relaxed);

        ObjectBuilder o;
        o.set("index", ev::fromDouble(static_cast<double>(i)));
        o.set("peak", ev::fromDouble(pk / 10000.0));
        o.set("rms", ev::fromDouble(rms / 10000.0));
        if (g_mic.wantSamples && g_mic.chunkFrames > 0) {
            const float* src = g_mic.sampleRing.data() + static_cast<size_t>(slot) * static_cast<size_t>(g_mic.chunkFrames);
            o.set("samples", makeFloat32Array(src, static_cast<size_t>(g_mic.chunkFrames)));
        }
        Value chunkVal = o.get();
        ev::call(g_mic.onChunk.get(), ev::undefined(), std::span<const Value>(&chunkVal, 1));
    }
    g_mic.lastFired = w;
}

void installMic() {
    Value broVal = ev::getGlobal("bro");
    ObjectBuilder broObj = ev::isObject(broVal) ? ObjectBuilder(broVal) : ObjectBuilder();

    ObjectBuilder mic;

    mic.def("start", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e) {
            return ev::throwError("bro.mic.start: audio engine not available");
        }

        int chunkFrames = 160;
        int targetRate = 16000;
        bool agc = false;
        bool live = true;
        bool samples = false;
        Value onChunkCb = ev::undefined();

        broaudio::MicTapConfig cfg;

        if (!a.empty() && ev::isObject(a[0])) {
            Value opt = a[0];

            Value cf = ev::getProperty(opt, "chunkFrames");
            if (!ev::isUndefined(cf) && !ev::isObject(cf)) chunkFrames = static_cast<int>(ev::toDouble(cf));

            Value tr = ev::getProperty(opt, "targetRate");
            if (!ev::isUndefined(tr) && !ev::isObject(tr)) targetRate = static_cast<int>(ev::toDouble(tr));

            Value agcV = ev::getProperty(opt, "agc");
            if (ev::isBool(agcV)) agc = ev::toBool(agcV);

            Value liveV = ev::getProperty(opt, "live");
            if (ev::isBool(liveV)) live = ev::toBool(liveV);

            Value sampV = ev::getProperty(opt, "samples");
            if (ev::isBool(sampV)) samples = ev::toBool(sampV);

            Value cb = ev::getProperty(opt, "onChunk");
            if (ev::isFunction(cb)) onChunkCb = cb;

            Value tp = ev::getProperty(opt, "targetPeak");
            if (!ev::isUndefined(tp) && !ev::isObject(tp)) cfg.agcCfg.targetPeak = static_cast<float>(ev::toDouble(tp));

            Value hl = ev::getProperty(opt, "halfLifeSec");
            if (!ev::isUndefined(hl) && !ev::isObject(hl)) cfg.agcCfg.halfLifeSec = static_cast<float>(ev::toDouble(hl));

            Value ng = ev::getProperty(opt, "noiseGate");
            if (!ev::isUndefined(ng) && !ev::isObject(ng)) cfg.agcCfg.noiseGate = static_cast<float>(ev::toDouble(ng));

            Value mg = ev::getProperty(opt, "maxGain");
            if (!ev::isUndefined(mg) && !ev::isObject(mg)) cfg.agcCfg.maxGain = static_cast<float>(ev::toDouble(mg));
        }

        if (chunkFrames < 0 || targetRate < 0) {
            return ev::throwError("bro.mic.start: chunkFrames and targetRate must be >= 0");
        }
        if (samples && chunkFrames <= 0) {
            return ev::throwError("bro.mic.start: opts.samples requires chunkFrames > 0");
        }

        shutdownActiveMic();

        cfg.chunkFrames = chunkFrames;
        cfg.targetRate = targetRate;
        cfg.agc = agc;

        g_mic.chunkFrames = chunkFrames;
        g_mic.wantSamples = samples;
        if (ev::isFunction(onChunkCb)) {
            g_mic.onChunk.set(onChunkCb);
        }
        if (samples) {
            g_mic.sampleRing.assign(static_cast<size_t>(kMicRing) * static_cast<size_t>(chunkFrames), 0.0f);
        }
        g_mic.writeCount.store(0, std::memory_order_relaxed);
        g_mic.dropped.store(0, std::memory_order_relaxed);
        g_mic.lastFired = 0;

        g_mic.tapId = e->addMicTap(cfg, [](const float* s, int n) {
            if (n <= 0) return;
            float peak = 0.0f, sumSq = 0.0f;
            for (int i = 0; i < n; ++i) {
                float absVal = std::fabs(s[i]);
                if (absVal > peak) peak = absVal;
                sumSq += s[i] * s[i];
            }
            float rms = std::sqrt(sumSq / static_cast<float>(n));
            uint64_t idx = g_mic.writeCount.load(std::memory_order_relaxed);
            int slot = static_cast<int>(idx % kMicRing);
            g_mic.peakRingX10000[slot].store(static_cast<int>(peak * 10000.0f), std::memory_order_relaxed);
            g_mic.rmsRingX10000[slot].store(static_cast<int>(rms * 10000.0f), std::memory_order_relaxed);
            if (g_mic.wantSamples) {
                int cf = g_mic.chunkFrames;
                int m = n < cf ? n : cf;
                float* dst = g_mic.sampleRing.data() + static_cast<size_t>(slot) * static_cast<size_t>(cf);
                std::memcpy(dst, s, static_cast<size_t>(m) * sizeof(float));
                if (m < cf) {
                    std::memset(dst + m, 0, static_cast<size_t>(cf - m) * sizeof(float));
                }
            }
            g_mic.writeCount.store(idx + 1, std::memory_order_release);
        });

        if (g_mic.tapId == broaudio::kInvalidMicTapId) {
            shutdownActiveMic();
            return ev::throwError("bro.mic.start: addMicTap failed");
        }

        if (live && !e->isMicCapturing()) {
            e->startMicCapture();
        }
        g_mic.active = true;
        return ev::undefined();
    });

    mic.def("stop", 0, [](Value, std::span<const Value>) -> Value {
        shutdownActiveMic();
        return ev::undefined();
    });

    mic.def("isActive", 0, [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(g_mic.active);
    });

    mic.def("engineRate", 0, [](Value, std::span<const Value>) -> Value {
        auto* e = getAudioEngine();
        return ev::fromDouble(e ? e->sampleRate() : 0);
    });

    mic.def("stats", 0, [](Value, std::span<const Value>) -> Value {
        auto* e = getAudioEngine();
        if (!g_mic.active || !e || g_mic.tapId == broaudio::kInvalidMicTapId) {
            return ev::null();
        }
        auto s = e->getMicTapStats(g_mic.tapId);
        ObjectBuilder res;
        res.set("framesDelivered", static_cast<double>(s.framesDelivered));
        res.set("samplesDelivered", static_cast<double>(s.samplesDelivered));
        res.set("rollingPeak", static_cast<double>(s.rollingPeak));
        res.set("chunkCount", static_cast<double>(g_mic.writeCount.load(std::memory_order_acquire)));
        res.set("dropped", static_cast<double>(g_mic.dropped.load(std::memory_order_relaxed)));
        res.set("chunkFrames", static_cast<double>(g_mic.chunkFrames));
        return res.get();
    });

    mic.def("levels", 1, [](Value, std::span<const Value> a) -> Value {
        int limit = kMicRing;
        if (!a.empty() && !ev::isUndefined(a[0]) && !ev::isObject(a[0])) {
            int maxC = i32At(a, 0);
            if (maxC >= 0 && maxC < limit) limit = maxC;
        }
        uint64_t w = g_mic.writeCount.load(std::memory_order_acquire);
        int avail = static_cast<int>(w < static_cast<uint64_t>(kMicRing) ? w : static_cast<uint64_t>(kMicRing));
        int count = avail < limit ? avail : limit;

        return hostArrayOf(static_cast<size_t>(count), [w, count](size_t k) {
            uint64_t idx = w - static_cast<uint64_t>(count) + static_cast<uint64_t>(k);
            int pk = g_mic.peakRingX10000[idx % kMicRing].load(std::memory_order_relaxed);
            return ev::fromDouble(pk / 10000.0);
        });
    });

    mic.def("feed", 2, [](Value, std::span<const Value> a) -> Value {
        if (!g_mic.active) {
            return ev::throwError("bro.mic.feed: mic capture not started");
        }
        auto* e = getAudioEngine();
        if (!e) {
            return ev::throwError("bro.mic.feed: audio engine not available");
        }
        if (e->isMicCapturing()) {
            return ev::throwError("bro.mic.feed: cannot feed while live mic capture is active");
        }
        if (a.empty()) return ev::undefined();

        std::vector<float> storage;
        const float* samples = nullptr;
        size_t count = 0;
        if (!floatData(a[0], storage, &samples, &count) || !samples || count == 0) {
            return ev::undefined();
        }

        int engRate = e->sampleRate();
        if (a.size() >= 2 && !ev::isUndefined(a[1]) && !ev::isObject(a[1])) {
            int sampleRate = i32At(a, 1);
            if (sampleRate > 0 && sampleRate != engRate) {
                std::string err = "bro.mic.feed: sampleRate=" + std::to_string(sampleRate) +
                                  " must equal engine rate=" + std::to_string(engRate);
                return ev::throwError(err);
            }
        }

        e->injectMicSamples(samples, static_cast<int>(count));
        return ev::undefined();
    });

    mic.def("drain", 0, [](Value, std::span<const Value>) -> Value {
        drainMicChunks();
        return ev::undefined();
    });

    broObj.set("mic", mic.get());
    ev::setGlobalValue("bro", broObj.get());
}

} // namespace broaudio::api
