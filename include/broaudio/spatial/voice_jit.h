#pragma once

#include "broaudio/spatial/spatial_chain.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace brass::codegen {
class KernelFunction;
}

namespace broaudio {

// Compiled per-voice chains (brass). The interpreted chain in spatial_chain.h
// is the reference and the fallback; the compiled path produces the same
// samples bit for bit (kernels are emitted without FMA contraction or
// reassociation, in the interpreter's operation order, so the parity test
// compares exactly).
//
// Two kinds of kernel, both shared by every voice that uses them:
//
//  - The air lane kernel runs the air stage for up to kAirLanes voice-channels
//    at once, one per SIMD lane (a stereo voice takes two lanes). The seven
//    sections run one after another over the block, so each section's state
//    lives in four vector registers.
//  - A chain kernel per chain *shape* runs every stage after air: delay,
//    gain/pan, head + occlusion, bus and send taps, in one loop per voice.
//
// Per-voice numbers travel in VoiceJitArgs (per block) and in the voice's own
// VoiceChainState, which the kernels and the glue read and write in place.
// Interpreted and compiled blocks can therefore alternate freely on one voice
// (a kernel that is not ready yet, a shape that changes, the A/B switch)
// without a seam.

// The shape of a voice chain. Stages run in this order:
//
//   [source] -> air -> delay -> gain/pan -> head+occlusion -> bus / send taps
//
// Gain/pan and the bus tap always exist. The source stage is a synthesis
// voice (synth/synth_graph.h) whose layers run their own kernels, one per
// layer shape; air runs in the lane kernel. So the chain kernel of a shape is
// the one of the same shape without source and air (chainKey()).
struct VoiceJitTopology {
    enum class Delay : uint8_t {
        None,
        // The delay line is settled (both smoothing one-poles and the read
        // delay sit exactly on the target): the smoother is skipped, the read
        // stays 4-point interpolated. A static source (and every one-shot's
        // onset block) runs this shape.
        Onset,
        // The delay follows a moving target through the two one-poles.
        Interpolated,
    };

    bool source = false;    // synthesis source stage
    bool stereo = false;    // two source channels (balance pan), else mono
    bool air = false;
    Delay delay = Delay::None;
    bool head = false;      // head shadow + occlusion
    bool send = false;      // aux send tap

    static constexpr int kKeys = 128;

    // Dense key in [0, kKeys).
    uint32_t key() const noexcept {
        uint32_t k = 0;
        if (source) k |= 1u << 0;
        if (air) k |= 1u << 1;
        if (delay != Delay::None) k |= 1u << 2;
        if (head) k |= 1u << 3;
        if (send) k |= 1u << 4;
        if (delay == Delay::Onset) k |= 1u << 5;
        if (stereo) k |= 1u << 6;
        return k;
    }
    // The key of the chain kernel that runs this shape.
    uint32_t chainKey() const noexcept { return key() & ~3u; }

    static VoiceJitTopology fromKey(uint32_t k) noexcept {
        VoiceJitTopology t;
        t.source = (k & (1u << 0)) != 0;
        t.air = (k & (1u << 1)) != 0;
        if (k & (1u << 2)) t.delay = (k & (1u << 5)) ? Delay::Onset : Delay::Interpolated;
        t.head = (k & (1u << 3)) != 0;
        t.send = (k & (1u << 4)) != 0;
        t.stereo = (k & (1u << 6)) != 0;
        return t;
    }
    // A key a chain kernel exists for: no source stage, no air, onset only
    // with a delay.
    static bool compilable(uint32_t k) noexcept {
        if (k >= static_cast<uint32_t>(kKeys) || (k & 3u)) return false;
        if ((k & (1u << 5)) && !(k & (1u << 2))) return false;
        return true;
    }
    bool operator==(const VoiceJitTopology& o) const noexcept { return key() == o.key(); }
};

// Per-block arguments of a chain kernel, filled by runVoiceChainJit. Plain
// data; the kernel addresses fields by offset. Per-sample parameters are read
// as p[i * stride]: stride 0 is one constant value, 1 a trajectory the glue
// computed with the interpreter's own ParamRamp code.
struct VoiceJitArgs {
    const float* ch0 = nullptr;     // planar source block (after air)
    const float* ch1 = nullptr;     // second channel (stereo shapes)
    float* bus = nullptr;           // interleaved stereo accumulation target
    float* send = nullptr;          // interleaved stereo send target
    float* line0 = nullptr;         // delay line channels
    float* line1 = nullptr;
    const float* gain = nullptr;    // playback gain x distance gain
    const float* sendAmt = nullptr; // send amount
    const float* panL = nullptr;    // pan gains
    const float* panR = nullptr;
    int32_t gainStride = 0;
    int32_t sendStride = 0;
    int32_t panStride = 0;
    int32_t delayMask = 0;
    float delayTarget = 0.0f;       // clamped to [delayLo, delayHi]
    float delayLo = 0.0f;
    float delayHi = 0.0f;
    float delaySmooth = 0.0f;
    float headGainL = 1.0f, headGainR = 1.0f;
    float headCoeffL = 0.0f, headCoeffR = 0.0f;
    // Per-sample parameters held constant for the block (stride 0) live here.
    float gainConst = 1.0f;
    float sendConst = 0.0f;
};

using VoiceJitFn = void (*)(const VoiceJitArgs* args, VoiceChainState* state, int32_t numFrames);

// The air lane kernel: `x` is kAirLanes interleaved lanes ([frame][lane]),
// filtered in place; `sections` is one block of lane state per air section
// (AirLaneSection, voice_jit_builder.h).
constexpr int kAirLanes = 8;
using AirJitFn = void (*)(float* x, float* sections, int32_t numFrames);

// One voice of a batch: its block parameters, state and planar source block
// (`ch[1]` only for stereo).
struct VoiceJitJob {
    const VoiceChainParams* p = nullptr;
    VoiceChainState* s = nullptr;
    float* ch[2] = {nullptr, nullptr};
};

// Audio-thread scratch for a batch of blocks of up to `maxFrames` frames:
// `lanes` holds kAirLanes x maxFrames floats, `traj` 4 x maxFrames.
struct VoiceJitScratch {
    float* lanes = nullptr;
    float* traj = nullptr;
    int maxFrames = 0;
};

// Floats per frame a mixer batching voices needs in all: each job's two
// source planes, the lane block and the trajectories.
constexpr int kVoiceBatchPlanes = 2 * kAirLanes + kAirLanes + 4;

// Batch capacity: a batch holds at most kAirLanes jobs, and the jobs with air
// hold at most kAirLanes channels between them.
int voiceJitAirLanes(const VoiceChainParams& p);

// The chain-kernel key this block runs as (the source stage and air excluded:
// they run before it, in their own kernels), or -1 when it has to run
// interpreted (a stereo source on a mono delay line).
int voiceJitShape(const VoiceChainParams& p, const VoiceChainState& s);

// The air stage of up to kAirLanes channels' worth of jobs (jobs without air
// are skipped) through the lane kernel. chainAir, bit for bit, on each.
// `laneScratch` holds kAirLanes x n floats.
void runAirLanesJit(AirJitFn fn, const VoiceJitJob* jobs, int count, float* laneScratch, int n);

// Every stage after air through a chain kernel of shape voiceJitShape(p, s):
// runVoiceChain minus the air stage, bit for bit. `ch` is only read; `traj` is
// 4 x n floats of scratch.
void runVoiceChainJit(VoiceJitFn fn, const VoiceChainParams& p, VoiceChainState& s,
                      float* const* ch, float* traj, int n);

class VoiceJitCache;

// A batch of voices in order, each exactly as runVoiceChain would run it
// (so buses accumulate in the same order): compiled kernels where published,
// interpreted stages where not (requesting the missing kernels). Synthesis
// sources render first, through their layer kernels. Returns the number of
// jobs that ran fully compiled.
int runVoiceBatch(VoiceJitCache& cache, const VoiceJitJob* jobs, int count,
                  const VoiceJitScratch& scratch, int n);

// True when this build carries a brass code generator for the host.
bool voiceJitBackendAvailable() noexcept;

// Kernel keys: chain kernels use VoiceJitTopology keys, the air lane kernel
// this one.
constexpr uint32_t kAirKernelKey = VoiceJitTopology::kKeys;
constexpr int kVoiceKernelSlots = VoiceJitTopology::kKeys + 1;

// The compiled kernel for `key`, compiled on first request and cached for the
// life of the process (kernels hold no per-engine data, so every engine
// shares them). Blocking; never call it on the audio thread. Null if the key
// has no kernel or there is no backend (then never retried).
std::shared_ptr<brass::codegen::KernelFunction> voiceKernelFor(uint32_t key);

// One engine's view of the kernels: a lock-free slot per key the audio thread
// reads, and a worker that compiles the keys the audio thread asked for and
// publishes them.
//
// The audio thread's only entry points are lookup() and lookupAir(): an
// acquire load of the slot and, on a miss, one relaxed fetch_or of a "wanted"
// bit (only the first time). The worker notices wanted bits within ~10 ms,
// compiles off the audio thread and publishes the entry point with a release
// store. Kernels never die while the engine lives, so there is nothing to
// retire.
class VoiceJitCache {
public:
    VoiceJitCache();
    ~VoiceJitCache();
    VoiceJitCache(const VoiceJitCache&) = delete;
    VoiceJitCache& operator=(const VoiceJitCache&) = delete;

    void shutdown();

    // Audio thread: the kernel for `key`, or null (requesting it).
    VoiceJitFn lookup(uint32_t key) noexcept { return reinterpret_cast<VoiceJitFn>(slot(key)); }
    AirJitFn lookupAir() noexcept { return reinterpret_cast<AirJitFn>(slot(kAirKernelKey)); }

    // Control thread: compile (or fetch) and publish now. Returns whether the
    // key has a published kernel.
    bool compileSync(uint32_t key);
    // The air kernel and every compilable chain shape; returns how many are
    // published.
    int compileAllSync();
    int publishedCount() const noexcept;

private:
    void* slot(uint32_t key) noexcept {
        void* f = slots_[key].load(std::memory_order_acquire);
        if (f) return f;
        const uint64_t bit = 1ull << (key & 63);
        std::atomic<uint64_t>& w = wanted_[key >> 6];
        if (!(w.load(std::memory_order_relaxed) & bit)) w.fetch_or(bit, std::memory_order_relaxed);
        return nullptr;
    }
    void workerLoop();

    static constexpr int kWords = (kVoiceKernelSlots + 63) / 64;
    std::atomic<void*> slots_[kVoiceKernelSlots] = {};
    std::atomic<uint64_t> wanted_[kWords] = {};
    uint64_t handled_[kWords] = {};   // worker only: wanted bits already served

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace broaudio
