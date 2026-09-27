#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace broaudio {

// Synthesis graphs: a voice driven by a small graph of generators and
// processors instead of a clip. The graph is the source stage at the head of
// the per-voice chain (spatial/spatial_chain.h, kStageSource).
//
// A graph is described declaratively (JSON; the JS binding hands in
// JSON.stringify of a plain object) and has two halves:
//
//  - its *shape*: per layer, the node kinds, their variants (wave, colour,
//    filter mode, ...) and which inputs are wired to other nodes. One brass
//    kernel is compiled per layer shape and shared by every voice of that
//    shape, in every graph that has it.
//  - its *parameters*: every number in the description (frequencies, times,
//    levels, gains, mode ratios, layer offsets). They live per voice, so two
//    voices of one graph with different parameters (a seed's jitter, a
//    caller's overrides) share the kernel.
//
// Every voice renders through the interpreter (the reference) or through the
// compiled kernels; both produce the same samples bit for bit, and output
// never depends on how a render is split into blocks. An offline render
// (renderSynth) therefore equals what the voice produces in the engine before
// the distance chain.
//
// The description format, node by node, is documented in bro's
// docs/audio-synth-graph-api.js.

// A description that does not parse or validate. The message starts with the
// path of the offending field ("nodes.body.cutoff: unknown node 'swep'").
// `range` marks a well-formed number outside its allowed range.
class SynthGraphError : public std::invalid_argument {
public:
    SynthGraphError(const std::string& message, bool range = false)
        : std::invalid_argument(message), range_(range) {}
    bool isRange() const noexcept { return range_; }

private:
    bool range_;
};

// One parameter: its name (the path of the number in the description,
// "body.cutoff", "bell.modes.2.ratio", "crack.offset"), its declared value,
// its jitter and the range any value is clamped into after jitter.
struct SynthParamInfo {
    std::string name;
    float value = 0.0f;
    float jitter = 0.0f;      // relative: value * (1 + jitter * u), u uniform in [-1, 1)
    float jitterAbs = 0.0f;   // absolute: + jitterAbs * u' (an independent u')
    float lo = 0.0f, hi = 0.0f;
};

struct SynthGraphData;   // src/synth/synth_plan.h
struct SynthVoiceData;   // src/synth/synth_voice_data.h
struct SynthLoopData;

// What makes one trigger of a graph: the seed of its jitter and noise, and
// overrides of declared parameter values (applied before jitter).
struct SynthTrigger {
    uint32_t seed = 0;
    bool jitter = true;                               // false: every u is 0
    std::vector<std::pair<int, float>> overrides;     // (param index, value)
};

// A seamless loop cut from a voice (renderSynth with `loop`, or a looping
// SynthVoice / Engine::playSynth with `loop`).
//
// The voice is rendered past `start` (the warm-up: the attack is over and
// filters, resonators and combs have settled) for length + crossfade frames,
// with no end: `duration`, the end of the envelopes and the silence floor
// are ignored. Loop sample i is voice sample start + i, except the first
// `crossfade` frames, where the voice's continuation past the loop's end
// (start + length + i) fades out as the start fades in. So the seam is the
// voice itself: the sample after the loop's last one is the next sample the
// voice produced, with every filter, resonator and comb state carried across
// it, and the blend sits inside the loop where both halves are the same
// sound.
//
// `snap` moves every periodic rate onto a whole number of cycles over the
// loop (at least one): constant oscillator and FM carrier frequencies (the
// FM ratio follows so the modulator is whole too) and constant impulse
// rates. A rate wired to a `mix` or `mul` of constants only is constant
// (folded): the first constant feeding it that can carry the change moves (a
// mix's first input with a non-zero weight, a mul's first factor). A rate
// that is a real signal (a sweep, an LFO) is not snapped. The voice's
// values() (and SynthGraph::values with the loop) report the snapped numbers.
//
// The crossfade is per layer: each layer's two halves are blended with the
// curve, and under Auto with that layer's own correlation, so a tonal layer
// and a noise layer each keep their level through the blend.
enum class SynthLoopCurve : uint8_t {
    Auto,     // per layer: equal power corrected for its halves' correlation (r = 1: equal gain)
    Power,    // equal power (uncorrelated halves: noise)
    Linear,   // equal gain (identical halves: snapped, settled tones)
};

struct SynthLoopOptions {
    double length = 0.0;       // seconds; > 0 makes a loop (at most 60 s)
    double crossfade = 0.05;   // seconds, clamped to [0, length / 2]
    double start = 0.25;       // seconds of the voice before the loop
    bool snap = true;
    SynthLoopCurve curve = SynthLoopCurve::Auto;
    // A looping voice's note-off: the loop crossfades (equal power, this
    // long) into the voice's natural release, rendered from the loop's seam
    // with its envelopes released. A graph without an amplitude envelope has
    // no release: the loop fades out over this time instead.
    double releaseFade = 0.02;

    bool enabled() const noexcept { return length > 0.0; }
};

class SynthGraph {
public:
    // Parses and validates; throws SynthGraphError.
    static std::shared_ptr<const SynthGraph> fromJson(std::string_view json);

    ~SynthGraph();
    SynthGraph(const SynthGraph&) = delete;
    SynthGraph& operator=(const SynthGraph&) = delete;

    const std::vector<SynthParamInfo>& params() const noexcept;
    // -1 for an unknown name.
    int paramIndex(std::string_view name) const noexcept;
    // Overrides from a JSON object {"name": number, ...}; throws
    // SynthGraphError for an unknown name, a non-number or a value outside
    // the parameter's range.
    std::vector<std::pair<int, float>> overridesFromJson(std::string_view json) const;

    // One trigger's parameter values as its voice resolves them (overrides,
    // jitter, the range) at `sampleRate`, and, for an enabled `loop` with
    // `snap`, snapped as that loop snaps them: what SynthVoice::values()
    // reports, without rendering anything. Snapping is idempotent, so these
    // values given back as overrides (jitter off) render the same loop.
    std::vector<float> values(const SynthTrigger& trigger, int sampleRate = 48000,
                              const SynthLoopOptions& loop = SynthLoopOptions{}) const;

    int layerCount() const noexcept;
    std::string layerId(int layer) const;
    // The canonical shape key of a layer (what its kernel is cached by).
    std::string layerShapeKey(int layer) const;
    // Whether every layer's kernel is published.
    bool compiled() const noexcept;
    // Compile every layer's kernel now, blocking (tens of ms per shape the
    // process has not compiled before). Returns whether all are published;
    // false when the build has no brass backend.
    bool precompile() const;
    // Ask the engines' compile workers for this graph's kernels (returns at
    // once; the audio thread asks by itself the first time it meets a shape).
    void requestKernels() const;

    const SynthGraphData& data() const noexcept { return *data_; }

private:
    explicit SynthGraph(std::unique_ptr<SynthGraphData> d);
    std::unique_ptr<SynthGraphData> data_;
};

// One sounding instance of a graph. Created (and all its memory allocated)
// on a control thread; render() is audio-thread safe (no allocation, no
// lock).
class SynthVoice {
public:
    SynthVoice(std::shared_ptr<const SynthGraph> graph, int sampleRate, const SynthTrigger& trigger);
    // A looping voice (`loop.enabled()`; otherwise the voice above). The loop
    // and its release tail are rendered here, on the calling thread, through
    // the published kernels when `compiled` (the samples are the same
    // either way); render() then plays the loop over and over until
    // release(), crossfades into the tail and finishes when the tail ends.
    SynthVoice(std::shared_ptr<const SynthGraph> graph, int sampleRate, const SynthTrigger& trigger,
               const SynthLoopOptions& loop, bool compiled = true);
    ~SynthVoice();
    SynthVoice(const SynthVoice&) = delete;
    SynthVoice& operator=(const SynthVoice&) = delete;

    // The next `n` frames of the voice (mono) into out[0..n), overwriting.
    // `compiled`: run layers through their kernels where published (a shape
    // without one runs interpreted and is requested); false interprets
    // everything. Returns true when every layer that ran, ran compiled.
    // Once finished() the output is silence.
    bool render(float* out, int n, bool compiled);

    // Finished: the amplitude envelopes are done and the output has decayed
    // below -100 dBFS over a 256-sample window, or the graph's duration is
    // reached. Samples from endSample() on are zero.
    bool finished() const noexcept;
    int64_t endSample() const noexcept;
    int64_t position() const noexcept;   // frames rendered so far

    // Note-off: every envelope that holds (an ADSR's sustain, a `hold`
    // segment) moves on to its release at the start of the next render.
    // Thread-safe.
    void release() noexcept { releaseRequested_.store(true, std::memory_order_relaxed); }

    // The voice's parameter values after overrides and jitter.
    const std::vector<float>& values() const noexcept;
    const SynthGraph& graph() const noexcept { return *graph_; }

    bool looping() const noexcept { return loop_ != nullptr; }
    // A looping voice's loop (one period, what renderSynth with `loop`
    // returns) and its release tail; empty for a voice that does not loop.
    const std::vector<float>& loopSamples() const noexcept;
    const std::vector<float>& releaseTail() const noexcept;

private:
    bool renderLoop(float* out, int n);

    std::shared_ptr<const SynthGraph> graph_;
    std::unique_ptr<SynthVoiceData> d_;
    std::unique_ptr<SynthLoopData> loop_;
    std::atomic<bool> releaseRequested_{false};
};

struct SynthRenderOptions {
    int sampleRate = 48000;
    SynthTrigger trigger;
    double maxSeconds = 10.0;   // a voice that never finishes is cut here (not a loop)
    bool compiled = true;       // compile the kernels (blocking, first time) and use them
    int blockSize = 1024;       // any size gives the same samples
    SynthLoopOptions loop;      // enabled: the result is one period of the loop
};

// Renders one trigger of `graph` offline: mono samples up to the voice's end
// (or maxSeconds), or with `loop` one period of the loop. Deterministic, and
// bit-identical to the voice in the engine before its distance chain (a
// looping voice plays exactly these samples, period after period). Thread-
// safe: any number of renders of one graph may run at once.
std::vector<float> renderSynth(const std::shared_ptr<const SynthGraph>& graph,
                               const SynthRenderOptions& options);

} // namespace broaudio
