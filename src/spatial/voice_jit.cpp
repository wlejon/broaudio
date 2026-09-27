// Compiled per-voice chains: the shape of a block, the glue that prepares the
// kernels' per-block arguments exactly as the interpreted stages would, the
// batch runner, the process-wide kernel cache and the per-engine lock-free
// slots.

#include "broaudio/spatial/voice_jit.h"
#include "broaudio/synth/oscillator.h"
#include "voice_jit_builder.h"
#include "../synth/synth_plan.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <unordered_map>

#if BROAUDIO_HAS_JIT
#include <brass/codegen/kernel_jit.hpp>
#endif

namespace broaudio {

namespace {

// The delay stage's clamp window (chainDelay).
inline void delayWindow(const PropagationDelayBuffer& buf, float maxDelay, float& lo, float& hi) {
    lo = 2.0f;
    hi = std::max(lo, std::min(maxDelay, static_cast<float>(buf.capacity - 4)));
}

inline int channelsOf(const VoiceChainParams& p) { return p.channels == 2 ? 2 : 1; }

} // namespace

int voiceJitAirLanes(const VoiceChainParams& p)
{
    return (p.stages & kStageAir) ? channelsOf(p) : 0;
}

int voiceJitShape(const VoiceChainParams& p, const VoiceChainState& s)
{
    VoiceJitTopology t;
    t.stereo = p.channels == 2;
    if ((p.stages & kStageDelay) && p.delayBuf) {
        const PropagationDelayBuffer& buf = *p.delayBuf;
        // chainDelay delays min(nch, buf.channels) channels; a stereo source on
        // a mono line passes its right channel undelayed. Rare and transient
        // (the line is reallocated for the clip): leave it interpreted.
        if (t.stereo && buf.channels < 2) return -1;
        float lo, hi;
        delayWindow(buf, p.delayMax, lo, hi);
        const float target = std::clamp(p.delayTarget, lo, hi);
        const bool snaps = s.delayBufSeen != &buf || !s.delayPrimed;
        const bool settled = s.delayS1 == target && s.delayS2 == target && s.delayOut == target;
        t.delay = (snaps || settled) ? VoiceJitTopology::Delay::Onset
                                     : VoiceJitTopology::Delay::Interpolated;
    }
    t.head = (p.stages & kStageHead) != 0;
    t.send = (p.stages & kStageSend) && p.send;
    return static_cast<int>(t.key());
}

void runAirLanesJit(AirJitFn fn, const VoiceJitJob* jobs, int count, float* x, int n)
{
    if (n <= 0) return;
    AirLaneSection sec[kAirSections] = {};
    struct Lane { VoiceChainState* s; float* ch; int c; };
    Lane lanes[kAirLanes];
    int used = 0;
    const float inv = 1.0f / static_cast<float>(n);
    for (int j = 0; j < count; ++j) {
        const VoiceChainParams& p = *jobs[j].p;
        if (!(p.stages & kStageAir)) continue;
        VoiceChainState& s = *jobs[j].s;
        // chainAir's prologue.
        if (!s.airPrimed) {
            std::memcpy(s.airPole, p.airPole, sizeof(s.airPole));
            std::memcpy(s.airMix, p.airMix, sizeof(s.airMix));
            std::memset(s.airZ, 0, sizeof(s.airZ));
            std::memset(s.airX, 0, sizeof(s.airX));
            s.airPrimed = true;
        }
        const int nch = channelsOf(p);
        for (int c = 0; c < nch && used < kAirLanes; ++c) {
            const int l = used++;
            lanes[l] = {&s, jobs[j].ch[c], c};
            for (int k = 0; k < kAirSections; ++k) {
                sec[k].p[l] = s.airPole[k];
                sec[k].dp[l] = (p.airPole[k] - s.airPole[k]) * inv;
                sec[k].m[l] = s.airMix[k];
                sec[k].dm[l] = (p.airMix[k] - s.airMix[k]) * inv;
                sec[k].z[l] = s.airZ[c][k];
                sec[k].xp[l] = s.airX[c][k];
            }
        }
    }
    if (used == 0) return;

    // Planar channels -> [frame][lane]; idle lanes run on zeros.
    if (used < kAirLanes) std::memset(x, 0, sizeof(float) * kAirLanes * static_cast<size_t>(n));
    for (int l = 0; l < used; ++l) {
        const float* src = lanes[l].ch;
        for (int i = 0; i < n; ++i) x[i * kAirLanes + l] = src[i];
    }
    fn(x, &sec[0].p[0], n);
    for (int l = 0; l < used; ++l) {
        float* dst = lanes[l].ch;
        for (int i = 0; i < n; ++i) dst[i] = x[i * kAirLanes + l];
        VoiceChainState& s = *lanes[l].s;
        for (int k = 0; k < kAirSections; ++k) {
            s.airZ[lanes[l].c][k] = sec[k].z[l];
            s.airX[lanes[l].c][k] = sec[k].xp[l];
        }
    }
    // chainAir's epilogue: the block ends on the targets.
    for (int j = 0; j < count; ++j) {
        const VoiceChainParams& p = *jobs[j].p;
        if (!(p.stages & kStageAir)) continue;
        std::memcpy(jobs[j].s->airPole, p.airPole, sizeof(p.airPole));
        std::memcpy(jobs[j].s->airMix, p.airMix, sizeof(p.airMix));
    }
}

void runVoiceChainJit(VoiceJitFn fn, const VoiceChainParams& p, VoiceChainState& s,
                      float* const* ch, float* traj, int n)
{
    if (n <= 0) return;
    const int nch = channelsOf(p);
    VoiceJitArgs a;
    a.ch0 = ch[0];
    a.ch1 = nch == 2 ? ch[1] : ch[0];
    a.bus = p.bus;
    const bool sendOn = (p.stages & kStageSend) && p.send;
    a.send = sendOn ? p.send : nullptr;

    // Delay: chainDelay's prologue.
    if ((p.stages & kStageDelay) && p.delayBuf) {
        PropagationDelayBuffer& buf = *p.delayBuf;
        float lo, hi;
        delayWindow(buf, p.delayMax, lo, hi);
        const float target = std::clamp(p.delayTarget, lo, hi);
        if (s.delayBufSeen != &buf) {
            s.delayBufSeen = &buf;
            s.writePos = 0;
            s.delayPrimed = false;
        }
        if (!s.delayPrimed) {
            s.delayS1 = s.delayS2 = s.delayOut = target;
            s.delayPrimed = true;
        }
        const int nc = std::min(nch, buf.channels);
        a.line0 = buf.channel(0);
        a.line1 = buf.channel(nc > 1 ? 1 : 0);
        a.delayMask = buf.mask;
        a.delayTarget = target;
        a.delayLo = lo;
        a.delayHi = hi;
        a.delaySmooth = p.delaySmooth;
    }

    // Gain/pan and send: the ramps run here, with ParamRamp's own code, into
    // per-sample trajectories; a settled ramp's next() is the identity, so it
    // is one constant (stride 0).
    s.distanceGain.follow(p.distanceGain);
    s.pan.follow(p.pan);
    float* gainT = traj;
    float* sendT = traj + n;
    float* panLT = traj + 2 * n;
    float* panRT = traj + 3 * n;
    if (s.gain.settled() && s.distanceGain.settled()) {
        a.gainConst = s.gain.value * s.distanceGain.value;
        a.gain = &a.gainConst;
        a.gainStride = 0;
    } else {
        for (int i = 0; i < n; ++i) gainT[i] = s.gain.next() * s.distanceGain.next();
        a.gain = gainT;
        a.gainStride = 1;
    }
    auto pickPan = [&s](float v) {
        if (!s.panValid || v != s.panCached) {
            panGains(v, s.panL, s.panR);
            s.panCached = v;
            s.panValid = true;
        }
    };
    if (s.pan.settled()) {
        pickPan(s.pan.value);
        a.panL = &s.panL;
        a.panR = &s.panR;
        a.panStride = 0;
    } else {
        for (int i = 0; i < n; ++i) {
            pickPan(s.pan.next());
            panLT[i] = s.panL;
            panRT[i] = s.panR;
        }
        a.panL = panLT;
        a.panR = panRT;
        a.panStride = 1;
    }
    if (sendOn) {
        if (s.send.settled()) {
            a.sendConst = s.send.value;
            a.sendAmt = &a.sendConst;
            a.sendStride = 0;
        } else {
            for (int i = 0; i < n; ++i) sendT[i] = s.send.next();
            a.sendAmt = sendT;
            a.sendStride = 1;
        }
    }

    a.headGainL = p.head.gainL;
    a.headGainR = p.head.gainR;
    a.headCoeffL = p.head.coeffL;
    a.headCoeffR = p.head.coeffR;

    fn(&a, &s, n);
}

int runVoiceBatch(VoiceJitCache& cache, const VoiceJitJob* jobs, int count,
                  const VoiceJitScratch& scratch, int n)
{
    if (n <= 0 || count <= 0) return 0;
    count = std::min(count, kAirLanes);
    // Synthesis sources first: every later stage reads their block.
    bool sourceCompiled[kAirLanes];
    for (int j = 0; j < count; ++j) {
        const VoiceChainParams& p = *jobs[j].p;
        sourceCompiled[j] = !(p.stages & kStageSource) || chainSource(p, jobs[j].ch[0], n, true);
    }
    int shapes[kAirLanes];
    VoiceJitJob airJobs[kAirLanes];
    bool inLanes[kAirLanes] = {};
    int nAir = 0, lanesUsed = 0;
    for (int j = 0; j < count; ++j) {
        shapes[j] = voiceJitShape(*jobs[j].p, *jobs[j].s);
        const int need = voiceJitAirLanes(*jobs[j].p);
        if (shapes[j] >= 0 && need > 0 && lanesUsed + need <= kAirLanes) {
            airJobs[nAir++] = jobs[j];
            lanesUsed += need;
            inLanes[j] = true;
        }
    }
    AirJitFn air = nAir ? cache.lookupAir() : nullptr;
    // Air touches only each voice's own block and state, so running every
    // voice's air first leaves the buses accumulating in the same order.
    if (air) runAirLanesJit(air, airJobs, nAir, scratch.lanes, n);

    int compiled = 0;
    float* L = scratch.traj;
    float* R = scratch.traj + n;
    for (int j = 0; j < count; ++j) {
        const VoiceChainParams& p = *jobs[j].p;
        VoiceChainState& s = *jobs[j].s;
        float* ch[2] = {jobs[j].ch[0], jobs[j].ch[1] ? jobs[j].ch[1] : jobs[j].ch[0]};
        if (shapes[j] < 0) {
            VoiceChainParams rest = p;
            rest.stages &= ~static_cast<uint32_t>(kStageSource);
            runVoiceChain(rest, s, ch, L, R, n);
            continue;
        }
        const bool hasAir = (p.stages & kStageAir) != 0;
        const bool airCompiled = hasAir && air && inLanes[j];
        if (hasAir && !airCompiled) chainAir(s, p.airPole, p.airMix, ch, channelsOf(p), n);
        if (VoiceJitFn fn = cache.lookup(static_cast<uint32_t>(shapes[j]))) {
            runVoiceChainJit(fn, p, s, ch, scratch.traj, n);
            if ((!hasAir || airCompiled) && sourceCompiled[j]) ++compiled;
        } else {
            VoiceChainParams rest = p;
            rest.stages &= ~static_cast<uint32_t>(kStageAir | kStageSource);
            runVoiceChain(rest, s, ch, L, R, n);
        }
    }
    return compiled;
}

bool voiceJitBackendAvailable() noexcept
{
#if BROAUDIO_HAS_JIT
    return true;
#else
    return false;
#endif
}

// --- Process-wide kernel cache ---

std::shared_ptr<brass::codegen::KernelFunction> voiceKernelFor(uint32_t key)
{
    if (!voiceJitBackendAvailable()) return nullptr;
    if (key != kAirKernelKey && !VoiceJitTopology::compilable(key)) return nullptr;
    struct Entry {
        std::shared_ptr<brass::codegen::KernelFunction> kernel;
        bool failed = false;
    };
    // Leaked on purpose: kernels published to an engine's slots must outlive
    // every engine, including ones destroyed during static teardown.
    static auto* cache = new std::unordered_map<uint32_t, Entry>();
    static std::mutex mutex;
    // One compile at a time; serialising keeps two engines from compiling the
    // same kernel twice.
    std::lock_guard<std::mutex> lock(mutex);
    Entry& e = (*cache)[key];
    if (e.kernel || e.failed) return e.kernel;
    try {
        e.kernel = key == kAirKernelKey ? buildAirLaneKernel()
                                        : buildVoiceKernel(VoiceJitTopology::fromKey(key));
    } catch (...) {
        e.kernel = nullptr;
    }
    if (!e.kernel) e.failed = true;
    return e.kernel;
}

// --- Per-engine slots and worker ---

namespace {

void* entryOf(const std::shared_ptr<brass::codegen::KernelFunction>& k)
{
#if BROAUDIO_HAS_JIT
    return k ? k->entry_point() : nullptr;
#else
    (void)k;
    return nullptr;
#endif
}

} // namespace

VoiceJitCache::VoiceJitCache()
{
    if (voiceJitBackendAvailable()) worker_ = std::thread(&VoiceJitCache::workerLoop, this);
}

VoiceJitCache::~VoiceJitCache()
{
    shutdown();
}

void VoiceJitCache::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool VoiceJitCache::compileSync(uint32_t key)
{
    if (key >= static_cast<uint32_t>(kVoiceKernelSlots)) return false;
    if (slots_[key].load(std::memory_order_acquire)) return true;
    void* f = entryOf(voiceKernelFor(key));
    if (f) slots_[key].store(f, std::memory_order_release);
    return f != nullptr;
}

int VoiceJitCache::compileAllSync()
{
    compileSync(kAirKernelKey);
    for (uint32_t k = 0; k < static_cast<uint32_t>(VoiceJitTopology::kKeys); ++k)
        if (VoiceJitTopology::compilable(k)) compileSync(k);
    return publishedCount();
}

int VoiceJitCache::publishedCount() const noexcept
{
    int c = 0;
    for (const auto& s : slots_)
        if (s.load(std::memory_order_relaxed)) ++c;
    return c;
}

void VoiceJitCache::workerLoop()
{
    // The audio thread cannot signal a condition variable without risking a
    // lock, so the worker looks at the wanted bits on a short timer.
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        cv_.wait_for(lock, std::chrono::milliseconds(10));
        if (stop_) break;
        for (int word = 0; word < kWords; ++word) {
            uint64_t pending = wanted_[word].load(std::memory_order_relaxed) & ~handled_[word];
            while (pending && !stop_) {
                const int bit = std::countr_zero(pending);
                pending &= pending - 1;
                handled_[word] |= 1ull << bit;
                const uint32_t key = static_cast<uint32_t>(word * 64 + bit);
                lock.unlock();
                compileSync(key);
                lock.lock();
            }
        }
        // Synthesis layer kernels (process-wide, requested by any engine's
        // audio thread or by a graph as it is played).
        lock.unlock();
        compilePendingSynthKernels();
        lock.lock();
    }
}

} // namespace broaudio
