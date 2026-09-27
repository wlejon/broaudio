#pragma once

// Internal structures of synthesis graphs (synth/synth_graph.h).
//
// A graph is a list of layers. Each layer has a LayerPlan: its nodes in
// evaluation order, where each input slot reads either an earlier node's
// signal or a per-voice constant, and the layout of everything the layer's
// kernel reads and carries:
//
//   k[]      per-voice float constants: constant input slots, and the numbers
//            the glue computes (envelope segment coefficients, resonator mode
//            coefficients, comb constants). k[0] = 1 / sampleRate, k[1] =
//            the layer's gain.
//   ki[]     per-voice int constants (a comb's delay and mask).
//   words[]  carried state, 4-byte words, float or int by the plan.
//   bufs[]   per-voice memory (comb delay lines).
//
// The plan depends only on the layer's shape, so every layer with one shape
// key has one layout and runs one kernel. What feeds the constants from the
// voice's parameters is per graph (LayerData).

#include "broaudio/synth/synth_graph.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace broaudio {

enum class SynthKind : uint8_t {
    Osc, Fm, Noise, Filter, Env, Shaper, Resonator, Comb, Mul, Mix,
};

enum class OscWave : uint8_t { Sine, Saw, Square, Triangle };
enum class NoiseColor : uint8_t { White, Pink, Brown };
enum class FilterMode : uint8_t { Lowpass, Highpass, Bandpass, Notch };
enum class ShaperMode : uint8_t { Tanh, Clip, Fold };
enum class SegCurve : uint8_t { Linear, Exp, Decay };

// Limits (validated when parsing).
constexpr int kSynthMaxLayers = 16;
constexpr int kSynthMaxNodes = 48;       // per layer
constexpr int kSynthMaxModes = 24;       // per resonator
constexpr int kSynthMaxMixInputs = 16;
constexpr int kSynthMaxSegments = 16;
constexpr int kSynthMaxWords = 512;      // carried state words per layer
constexpr int kSynthMaxK = 1024;         // float constants per layer
constexpr int kSynthMaxKi = 64;
constexpr int kSynthMaxBufs = 8;
constexpr int kSynthMaxPrep = 4;         // prep values per node

// An input slot: an earlier node's output, or constant k[k].
struct SlotPlan {
    int16_t node = -1;
    int16_t k = -1;
    bool wired() const noexcept { return node >= 0; }
};

// Slot order per kind (the last slot is always the node's output gain):
//   Osc       freq, pm, pw, gain
//   Fm        freq, ratio, index, gain         kBase: feedback (radians)
//   Noise     gain
//   Filter    input, cutoff, q, gain
//   Env       gain                             kBase: m, c
//   Shaper    input, drive, gain
//   Resonator input, gain                      kBase: 3 per mode (b, a1, a2)
//   Comb      input, gain                      kBase: frac, feedback, damp; ki: delay, mask; buf
//   Mul       a, b, gain
//   Mix       in 0..N-1, gain                  kBase: N weights
struct NodePlan {
    SynthKind kind = SynthKind::Mul;
    uint8_t variant = 0;
    uint8_t count = 0;            // resonator modes / mix inputs
    std::vector<SlotPlan> slots;
    int kBase = -1;
    int kiBase = -1;
    int buf = -1;
    int sBase = -1;               // first state word
    int sCount = 0;

    const SlotPlan& gain() const { return slots.back(); }
};

struct LayerPlan {
    std::vector<NodePlan> nodes;
    int output = 0;
    int kCount = 2;
    int kiCount = 0;
    int bufCount = 0;
    int words = 0;
    std::vector<uint8_t> wordIsInt;
    std::string key;

    static constexpr int kInvFs = 0;
    static constexpr int kGain = 1;
};

// A layer kernel's per-call arguments. The kernel reads out[i] and writes
// out[i] + y * gain for each of its n frames.
struct SynthKernelArgs {
    float* out = nullptr;
    const float* k = nullptr;
    const int32_t* ki = nullptr;
    float* const* bufs = nullptr;
};

using SynthKernelFn = void (*)(const SynthKernelArgs* args, uint32_t* words, int32_t n);

// One interned layer shape: the plan and, once compiled, its kernel. Lives
// for the process (the registry is never freed), shared by every graph.
struct SynthShape {
    LayerPlan plan;
    mutable std::atomic<void*> entry{nullptr};
    mutable std::atomic<bool> wanted{false};
    mutable std::atomic<bool> failed{false};

    SynthKernelFn kernel() const noexcept {
        return reinterpret_cast<SynthKernelFn>(entry.load(std::memory_order_acquire));
    }
};

// The shape for a plan (plan.key filled), interned process-wide.
const SynthShape* internSynthShape(LayerPlan&& plan);
// Audio-thread safe: note that `shape` should be compiled (the engines'
// compile workers pick it up). Lock-free.
void requestSynthKernel(const SynthShape& shape) noexcept;
// Blocking compile + publish; true when published.
bool compileSynthKernelSync(const SynthShape& shape);
// Compile every requested shape not yet compiled (the engines' workers call
// this on their timer). Cheap when nothing new was requested.
void compilePendingSynthKernels();
// Emit + compile the kernel for a plan (synth_jit.cpp); null without brass.
void* buildSynthKernel(const LayerPlan& plan, std::shared_ptr<void>& keepAlive);

// The interpreted layer: exactly what the kernel computes.
void runLayerInterpreted(const LayerPlan& plan, const SynthKernelArgs& args, uint32_t* words, int n);

// --- Per-graph data --------------------------------------------------------

struct SegDef {
    int time = -1;       // param index (seconds)
    int level = -1;      // param index
    SegCurve curve = SegCurve::Linear;
};

struct EnvDef {
    int node = 0;        // node index in the layer
    bool amp = true;     // counts toward the voice's end (env); false: sweep
    int start = -1;      // param: level before the first segment
    std::vector<SegDef> segs;
    int hold = -1;       // hold after segment `hold` until release
    int gate = -1;       // param: release time (seconds from layer start), or -1
};

struct ModeDef { int ratio = -1, decay = -1, gain = -1; };

struct ResDef {
    int node = 0;
    int freq = -1;
    std::vector<ModeDef> modes;
};

struct CombDef {
    int node = 0;
    int freq = -1, feedback = -1, damp = -1;
};

struct LayerData {
    std::string id;
    const SynthShape* shape = nullptr;
    std::vector<std::pair<int, int>> feeds;   // (k index, param index): k = value
    int offset = -1, gain = -1;               // params
    std::vector<EnvDef> envs;
    std::vector<ResDef> res;
    std::vector<CombDef> combs;
    std::vector<int> rngWords;                // noise generators' rng state words
};

struct SynthGraphData {
    std::vector<SynthParamInfo> params;
    std::unordered_map<std::string, int> paramIndex;
    std::vector<LayerData> layers;
    int duration = -1;                        // param, or -1
    int ampEnvs = 0;
};

// Parse + validate + plan (synth_parse.cpp). Throws SynthGraphError.
std::unique_ptr<SynthGraphData> parseSynthGraph(std::string_view json);

// Canonical key and layout of a plan whose nodes, slots' wiring, kinds,
// variants and counts are set (synth_parse.cpp).
void layoutSynthPlan(LayerPlan& plan);

} // namespace broaudio
