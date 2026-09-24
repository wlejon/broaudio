#include "api.h"
#include "host_audio_internal.h"
#include "object_builder.h"

#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace broaudio::api {

namespace {

constexpr int kMicRing = 4096;
// Sample-ring budget in floats (64 MB). A large chunkFrames gets fewer sample
// slots rather than a kMicRing * chunkFrames allocation (16 GB at the cap).
constexpr size_t kSampleRingBudget = size_t{1} << 24;

// Relaxed element access for the PCM ring. std::atomic_ref where the library
// has it; Apple's libc++ only ships it from Xcode 16, so older ones use the
// builtins atomic_ref is itself built on (MSVC's STL always has atomic_ref).
inline void relaxedStore(float& dst, float v) {
#if defined(__cpp_lib_atomic_ref)
    std::atomic_ref<float>(dst).store(v, std::memory_order_relaxed);
#else
    __atomic_store(&dst, &v, __ATOMIC_RELAXED);
#endif
}

inline float relaxedLoad(const float& src) {
#if defined(__cpp_lib_atomic_ref)
    return std::atomic_ref<float>(const_cast<float&>(src)).load(std::memory_order_relaxed);
#else
    float v;
    __atomic_load(&src, &v, __ATOMIC_RELAXED);
    return v;
#endif
}

// One bro.mic.start()'s ring, shared by the main thread and the tap callback.
// The callback owns a reference, so a callback still running on the audio
// thread after removeMicTap (the tap list is reclaimed by QSBR, not
// synchronously) writes into the session it started with — never into a
// freed ring or one resized by a restart.
//
// Each chunk slot is a seqlock whose sequence encodes the chunk index: the
// writer publishes 2*idx+1 while writing chunk idx and 2*idx+2 when done. A
// reader of chunk i expects exactly 2*i+2 before and after its copy; any other
// value means the writer lapped the slot (it only moves forward), so chunk i
// is gone and a retry would only yield a newer chunk under i's index — the
// chunk counts as dropped instead. PCM is copied element-wise through
// relaxed atomic_ref accesses, so the overlap a lap can cause is not a data
// race. The audio thread never waits.
struct MicSession {
    MicSession(int chunkFrames_, bool wantSamples_)
        : chunkFrames(chunkFrames_), wantSamples(wantSamples_ && chunkFrames_ > 0)
    {
        if (wantSamples) {
            const size_t cf = static_cast<size_t>(chunkFrames);
            sampleSlots = static_cast<int>(std::min<size_t>(
                kMicRing, std::max<size_t>(8, kSampleRingBudget / cf)));
            sampleRing.assign(static_cast<size_t>(sampleSlots) * cf, 0.0f);
            sampleSeq = std::make_unique<std::atomic<uint64_t>[]>(static_cast<size_t>(sampleSlots));
        }
    }

    const int chunkFrames;
    const bool wantSamples;
    // The sample ring can have fewer slots than the level ring, so it has
    // its own sequence per slot, on the same protocol.
    int sampleSlots = 0;

    std::atomic<uint64_t> slotSeq[kMicRing]{};
    std::atomic<int> peakRingX10000[kMicRing]{};
    std::atomic<int> rmsRingX10000[kMicRing]{};
    std::unique_ptr<std::atomic<uint64_t>[]> sampleSeq;  // value-initialized: 0
    std::vector<float> sampleRing;
    std::atomic<uint64_t> writeCount{0};

    // Main thread only.
    uint64_t lastFired = 0;
    uint64_t dropped = 0;

    // Audio thread (the tap callback) only: the single writer.
    void write(const float* s, int n) {
        float peak = 0.0f, sumSq = 0.0f;
        for (int i = 0; i < n; ++i) {
            float absVal = std::fabs(s[i]);
            if (absVal > peak) peak = absVal;
            sumSq += s[i] * s[i];
        }
        float rms = std::sqrt(sumSq / static_cast<float>(n));

        const uint64_t idx = writeCount.load(std::memory_order_relaxed);
        const int slot = static_cast<int>(idx % kMicRing);
        const size_t sslot = wantSamples ? static_cast<size_t>(idx % static_cast<uint64_t>(sampleSlots)) : 0;
        slotSeq[slot].store(2 * idx + 1, std::memory_order_relaxed);
        if (wantSamples) sampleSeq[sslot].store(2 * idx + 1, std::memory_order_relaxed);
        // Orders the odd stores before every payload store below.
        std::atomic_thread_fence(std::memory_order_release);
        peakRingX10000[slot].store(static_cast<int>(peak * 10000.0f), std::memory_order_relaxed);
        rmsRingX10000[slot].store(static_cast<int>(rms * 10000.0f), std::memory_order_relaxed);
        if (wantSamples) {
            const int cf = chunkFrames;
            const int m = n < cf ? n : cf;
            float* dst = sampleRing.data() + sslot * static_cast<size_t>(cf);
            for (int k = 0; k < m; ++k)
                relaxedStore(dst[k], s[k]);
            for (int k = m; k < cf; ++k)
                relaxedStore(dst[k], 0.0f);
            sampleSeq[sslot].store(2 * idx + 2, std::memory_order_release);
        }
        slotSeq[slot].store(2 * idx + 2, std::memory_order_release);
        writeCount.store(idx + 1, std::memory_order_release);
    }

    // Main thread: read chunk i (i < an acquire-loaded writeCount). False when
    // the writer has lapped it. `samples` is filled only when wantSamples.
    bool read(uint64_t i, int& peak, int& rms, std::vector<float>& samples) const {
        const int slot = static_cast<int>(i % kMicRing);
        const size_t sslot = wantSamples ? static_cast<size_t>(i % static_cast<uint64_t>(sampleSlots)) : 0;
        const uint64_t want = 2 * i + 2;
        if (slotSeq[slot].load(std::memory_order_acquire) != want) return false;
        if (wantSamples && sampleSeq[sslot].load(std::memory_order_acquire) != want) return false;
        peak = peakRingX10000[slot].load(std::memory_order_relaxed);
        rms = rmsRingX10000[slot].load(std::memory_order_relaxed);
        if (wantSamples) {
            const size_t cf = static_cast<size_t>(chunkFrames);
            samples.resize(cf);
            const float* src = sampleRing.data() + sslot * cf;
            for (size_t k = 0; k < cf; ++k)
                samples[k] = relaxedLoad(src[k]);
        }
        // Orders the payload loads before the re-checks: a payload load that
        // saw a lapping write makes that write's odd sequence visible below.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slotSeq[slot].load(std::memory_order_relaxed) != want) return false;
        if (wantSamples && sampleSeq[sslot].load(std::memory_order_relaxed) != want) return false;
        return true;
    }
};

struct MicState {
    std::shared_ptr<MicSession> session;  // null while stopped
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
    // The callback's own reference keeps the ring alive for a callback that
    // is still in flight; this only drops the main thread's.
    g_mic.session.reset();
    g_mic.active = false;
}

} // namespace

void drainMicChunks() {
    // The host's once-per-frame call into broaudio: settle background clip
    // loads here too (api.h tickAsyncJobs), so createClipFromFileAsync
    // resolves without the host pumping a second thing.
    tickAsyncJobs();
    if (!g_mic.active || !g_mic.session) return;
    // Held for the whole batch: a chunk callback may stop or restart the mic.
    std::shared_ptr<MicSession> sess = g_mic.session;
    uint64_t w = sess->writeCount.load(std::memory_order_acquire);
    if (w == sess->lastFired) return;
    if (!ev::isFunction(g_mic.onChunk.get())) {
        sess->lastFired = w;
        return;
    }

    uint64_t start = sess->lastFired;
    if (w - start > static_cast<uint64_t>(kMicRing)) {
        sess->dropped += w - start - kMicRing;
        start = w - kMicRing;
    }
    std::vector<float> chunkBuf;
    for (uint64_t i = start; i < w; ++i) {
        // A chunk callback may have called bro.mic.stop() (or restarted
        // with a new handler): stop delivering the old batch then.
        if (!g_mic.active || g_mic.session != sess || !ev::isFunction(g_mic.onChunk.get()))
            return;
        sess->lastFired = i + 1;
        int pk = 0, rms = 0;
        if (!sess->read(i, pk, rms, chunkBuf)) {
            // Overwritten while earlier chunks of this batch ran their
            // callbacks: the writer lapped it.
            sess->dropped++;
            continue;
        }

        ObjectBuilder o;
        o.set("index", ev::fromDouble(static_cast<double>(i)));
        o.set("peak", ev::fromDouble(pk / 10000.0));
        o.set("rms", ev::fromDouble(rms / 10000.0));
        if (sess->wantSamples) {
            o.set("samples", makeFloat32Array(chunkBuf.data(), chunkBuf.size()));
        }
        Value chunkVal = o.get();
        ev::call(g_mic.onChunk.get(), ev::undefined(), std::span<const Value>(&chunkVal, 1));
    }
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
        // Held rooted: the option reads after it allocate.
        ev::Persistent onChunkCb;

        broaudio::MicTapConfig cfg;

        if (!a.empty() && ev::isObject(a[0])) {
            // Read every option off a[0] (rooted) itself; a local copy of it
            // would be stale after the first getProperty.
            const Value& opt = a[0];

            Value cf = ev::getProperty(opt, "chunkFrames");
            if (!ev::isUndefined(cf) && !ev::isObject(cf)) chunkFrames = saturateI32(ev::toDouble(cf));

            Value tr = ev::getProperty(opt, "targetRate");
            if (!ev::isUndefined(tr) && !ev::isObject(tr)) targetRate = saturateI32(ev::toDouble(tr));

            Value agcV = ev::getProperty(opt, "agc");
            if (ev::isBool(agcV)) agc = ev::toBool(agcV);

            Value liveV = ev::getProperty(opt, "live");
            if (ev::isBool(liveV)) live = ev::toBool(liveV);

            Value sampV = ev::getProperty(opt, "samples");
            if (ev::isBool(sampV)) samples = ev::toBool(sampV);

            Value cb = ev::getProperty(opt, "onChunk");
            if (ev::isFunction(cb)) onChunkCb.set(cb);

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
        // The sample ring holds kMicRing chunks; an absurd chunk size would
        // size an allocation that throws out of the binding.
        constexpr int kMaxChunkFrames = 1 << 20;
        constexpr int kMaxTargetRate = 768000;
        if (chunkFrames > kMaxChunkFrames || targetRate > kMaxTargetRate) {
            return ev::throwRangeError("bro.mic.start: chunkFrames must be <= " +
                                       std::to_string(kMaxChunkFrames) +
                                       " and targetRate <= " + std::to_string(kMaxTargetRate));
        }
        if (samples && chunkFrames <= 0) {
            return ev::throwError("bro.mic.start: opts.samples requires chunkFrames > 0");
        }

        shutdownActiveMic();

        cfg.chunkFrames = chunkFrames;
        cfg.targetRate = targetRate;
        cfg.agc = agc;

        if (ev::isFunction(onChunkCb.get())) {
            g_mic.onChunk.set(onChunkCb.get());
        }
        auto sess = std::make_shared<MicSession>(chunkFrames, samples);
        g_mic.session = sess;

        g_mic.tapId = e->addMicTap(cfg, [sess](const float* s, int n) {
            if (n > 0) sess->write(s, n);
        });

        if (g_mic.tapId == broaudio::kInvalidMicTapId) {
            // Not active yet, so shutdownActiveMic would skip this.
            g_mic.session.reset();
            g_mic.onChunk.set(ev::undefined());
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
        const MicSession* sess = g_mic.session.get();
        res.set("chunkCount", static_cast<double>(sess ? sess->writeCount.load(std::memory_order_acquire) : 0));
        res.set("dropped", static_cast<double>(sess ? sess->dropped : 0));
        res.set("chunkFrames", static_cast<double>(sess ? sess->chunkFrames : 0));
        return res.get();
    });

    mic.def("levels", 1, [](Value, std::span<const Value> a) -> Value {
        int limit = kMicRing;
        if (!a.empty() && !ev::isUndefined(a[0]) && !ev::isObject(a[0])) {
            int maxC = i32At(a, 0);
            if (maxC >= 0 && maxC < limit) limit = maxC;
        }
        // Held across the array build (which allocates and so may run code
        // that stops the mic).
        std::shared_ptr<MicSession> sess = g_mic.session;
        uint64_t w = sess ? sess->writeCount.load(std::memory_order_acquire) : 0;
        int avail = static_cast<int>(w < static_cast<uint64_t>(kMicRing) ? w : static_cast<uint64_t>(kMicRing));
        int count = avail < limit ? avail : limit;

        return hostArrayOf(static_cast<size_t>(count), [&sess, w, count](size_t k) {
            uint64_t idx = w - static_cast<uint64_t>(count) + static_cast<uint64_t>(k);
            // Level meter: a lapped slot just reads a newer level.
            int pk = sess->peakRingX10000[idx % kMicRing].load(std::memory_order_relaxed);
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
        if (!readFloatArrayArg(a[0], FloatArrayArg::Float32OrPlain, "bro.mic.feed: samples", storage)) {
            return ev::undefined();
        }
        if (storage.empty()) return ev::undefined();
        const float* samples = storage.data();
        size_t count = storage.size();

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
