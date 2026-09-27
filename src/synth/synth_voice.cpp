// Synthesis voices: a graph's parameters resolved for one trigger (overrides,
// then seeded jitter), the per-voice constants and memory of each layer, and
// the glue that runs the layers block by block.
//
// The glue splits a block into sub-blocks at every event (an envelope segment
// ending, a gate, a layer starting) so that inside a sub-block every
// envelope is a fixed recurrence y = y * m + c the kernel runs without
// branches. Events fall on voice-time samples, never on block boundaries, and
// all state crosses sub-blocks in memory, exactly, so the samples a voice
// produces do not depend on how its renders are sized: an offline render and
// the engine's voice agree bit for bit.

#include "synth_plan.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define BROAUDIO_SYNTH_SSE 1
#endif

namespace broaudio {

namespace {

constexpr int64_t kInf = INT64_MAX / 4;
constexpr int kTailWindow = 256;
constexpr float kTailFloor = 1e-5f;   // -100 dBFS

// Flush-to-zero + denormals-are-zero for the render, whatever the calling
// thread's mode: the interpreter, the kernels and every thread that renders
// (audio thread, offline workers) then agree on the decaying tails.
struct FpModeGuard {
#if BROAUDIO_SYNTH_SSE
    unsigned saved;
    FpModeGuard() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }
    ~FpModeGuard() { _mm_setcsr(saved); }
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t saved;
    FpModeGuard() {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved));
        uint64_t v = saved | (1ull << 24);
        __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
    }
    ~FpModeGuard() { __asm__ __volatile__("msr fpcr, %0" : : "r"(saved)); }
#endif
};

inline uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Uniform in [-1, 1), a pure function of (seed, i).
inline float unitNoise(uint32_t seed, uint32_t i)
{
    const uint32_t h = mix32(mix32(seed * 0x9E3779B9u + 0x632BE5ABu) ^ (i * 0x85EBCA6Bu + 0x27D4EB2Fu));
    return static_cast<float>(static_cast<int32_t>(h) >> 8) * 1.1920928955078125e-07f;
}

inline float wordF(const uint32_t* w, int i)
{
    float f;
    std::memcpy(&f, &w[i], sizeof f);
    return f;
}

inline void setWordF(uint32_t* w, int i, float f) { std::memcpy(&w[i], &f, sizeof f); }

} // namespace

struct SynthEnvRun {
    const EnvDef* def = nullptr;
    int word = 0;        // y
    int kBase = 0;       // m, c
    int seg = 0;
    int64_t remaining = 0;
    int64_t gateAt = -1;
    bool holding = false;
    bool done = false;
    bool released = false;
};

struct SynthLayerRun {
    const LayerData* L = nullptr;
    const SynthShape* shape = nullptr;
    std::vector<float> k;
    std::vector<int32_t> ki;
    std::vector<uint32_t> words;
    std::vector<std::vector<float>> bufMem;
    std::vector<float*> bufs;
    std::vector<SynthEnvRun> envs;
    int64_t offset = 0;
    int64_t time = 0;
    bool started = false;
};

struct SynthVoiceData {
    int fs = 48000;
    std::vector<float> values;
    std::vector<SynthLayerRun> layers;
    int64_t pos = 0;
    int64_t end = -1;
    int64_t duration = -1;
    bool done = false;
    int ampPending = 0;
    bool ampDone = false;
    int64_t ampDoneAt = -1;
    float winPeak = 0.0f;
    int winFill = 0;

    float level(int param) const { return param >= 0 ? values[param] : 0.0f; }
    int64_t frames(int param) const {
        return static_cast<int64_t>(std::llround(static_cast<double>(std::max(0.0f, values[param])) * fs));
    }

    void finish(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt);
    void enter(SynthLayerRun& lr, SynthEnvRun& e, int s, int64_t lt);
    void release(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt);
    void advance(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt);
    bool runLayer(SynthLayerRun& lr, float* out, int n, bool compiled);
};

void SynthVoiceData::finish(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt)
{
    e.done = true;
    e.holding = false;
    e.remaining = kInf;
    e.seg = static_cast<int>(e.def->segs.size());
    lr.k[e.kBase] = 1.0f;
    lr.k[e.kBase + 1] = 0.0f;
    if (e.def->amp) {
        ampDoneAt = std::max(ampDoneAt, lr.offset + lt);
        if (--ampPending == 0) ampDone = true;
    }
}

// Start segment `s` from the envelope's current value, skipping empty
// segments (their level is reached at once).
void SynthVoiceData::enter(SynthLayerRun& lr, SynthEnvRun& e, int s, int64_t lt)
{
    const EnvDef& d = *e.def;
    uint32_t* w = lr.words.data();
    for (;;) {
        if (d.hold >= 0 && s == d.hold + 1 && !e.released) {
            // The sustain point. Holding silence with nothing to release it
            // is the end of a percussive envelope.
            if (d.gate < 0 && wordF(w, e.word) == 0.0f) { finish(lr, e, lt); return; }
            e.seg = s;
            e.holding = true;
            e.remaining = kInf;
            lr.k[e.kBase] = 1.0f;
            lr.k[e.kBase + 1] = 0.0f;
            return;
        }
        if (s >= static_cast<int>(d.segs.size())) { finish(lr, e, lt); return; }
        const SegDef& sd = d.segs[s];
        const int64_t n = frames(sd.time);
        const float target = level(sd.level);
        if (n <= 0) {
            setWordF(w, e.word, target);
            ++s;
            continue;
        }
        const float y0 = wordF(w, e.word);
        float m = 1.0f, c = 0.0f;
        SegCurve curve = sd.curve;
        if (curve == SegCurve::Exp && !(y0 != 0.0f && target != 0.0f && (y0 > 0.0f) == (target > 0.0f)))
            curve = SegCurve::Linear;
        switch (curve) {
        case SegCurve::Linear:
            c = (target - y0) / static_cast<float>(n);
            break;
        case SegCurve::Exp:
            m = static_cast<float>(std::pow(static_cast<double>(target) / y0, 1.0 / static_cast<double>(n)));
            break;
        case SegCurve::Decay:
            // 60 dB of the distance covered over the segment, then a snap.
            m = static_cast<float>(std::exp(-6.907755278982137 / static_cast<double>(n)));
            c = target * (1.0f - m);
            break;
        }
        e.seg = s;
        e.holding = false;
        e.remaining = n;
        lr.k[e.kBase] = m;
        lr.k[e.kBase + 1] = c;
        return;
    }
}

void SynthVoiceData::release(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt)
{
    if (e.released || e.done) return;
    e.released = true;
    if (e.def->hold >= 0 && (e.holding || e.seg <= e.def->hold)) enter(lr, e, e.def->hold + 1, lt);
}

void SynthVoiceData::advance(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt)
{
    for (;;) {
        if (!e.released && e.gateAt >= 0 && lt >= e.gateAt) {
            release(lr, e, lt);
            continue;
        }
        if (!e.done && !e.holding && e.remaining == 0) {
            setWordF(lr.words.data(), e.word, level(e.def->segs[e.seg].level));
            enter(lr, e, e.seg + 1, lt);
            continue;
        }
        return;
    }
}

bool SynthVoiceData::runLayer(SynthLayerRun& lr, float* out, int n, bool compiled)
{
    int p = 0;
    if (!lr.started) {
        if (lr.offset >= pos + n) return true;
        p = static_cast<int>(std::max<int64_t>(0, lr.offset - pos));
        lr.started = true;
    }
    bool allCompiled = true;
    const LayerPlan& plan = lr.shape->plan;
    SynthKernelArgs a;
    a.k = lr.k.data();
    a.ki = lr.ki.data();
    a.bufs = lr.bufs.data();
    while (p < n) {
        const int64_t lt = lr.time;
        int64_t sub = n - p;
        for (SynthEnvRun& e : lr.envs) {
            advance(lr, e, lt);
            if (!e.done && !e.holding) sub = std::min(sub, e.remaining);
            if (!e.released && e.gateAt >= 0) sub = std::min(sub, e.gateAt - lt);
        }
        const int m = static_cast<int>(sub);
        a.out = out + p;
        SynthKernelFn fn = compiled ? lr.shape->kernel() : nullptr;
        if (fn) {
            fn(&a, lr.words.data(), m);
        } else {
            if (compiled) {
                requestSynthKernel(*lr.shape);
                allCompiled = false;
            }
            runLayerInterpreted(plan, a, lr.words.data(), m);
        }
        for (SynthEnvRun& e : lr.envs)
            if (!e.done && !e.holding) e.remaining -= m;
        lr.time += m;
        p += m;
    }
    return allCompiled;
}

// --- SynthVoice ---------------------------------------------------------------

SynthVoice::SynthVoice(std::shared_ptr<const SynthGraph> graph, int sampleRate, const SynthTrigger& t)
    : graph_(std::move(graph)), d_(std::make_unique<SynthVoiceData>())
{
    const SynthGraphData& g = graph_->data();
    SynthVoiceData& v = *d_;
    v.fs = sampleRate > 0 ? sampleRate : 48000;
    const double fs = v.fs;

    // Parameters: declared value (or override), then jitter, then the range.
    v.values.resize(g.params.size());
    for (size_t i = 0; i < g.params.size(); ++i) v.values[i] = g.params[i].value;
    for (const auto& [idx, val] : t.overrides)
        if (idx >= 0 && idx < static_cast<int>(v.values.size())) v.values[idx] = val;
    for (size_t i = 0; i < g.params.size(); ++i) {
        const SynthParamInfo& p = g.params[i];
        float x = v.values[i];
        if (t.jitter) {
            const uint32_t id = static_cast<uint32_t>(i);
            if (p.jitter > 0.0f) x = x * (1.0f + p.jitter * unitNoise(t.seed, 2 * id));
            if (p.jitterAbs > 0.0f) x = x + p.jitterAbs * unitNoise(t.seed, 2 * id + 1);
        }
        v.values[i] = std::clamp(x, p.lo, p.hi);
    }
    if (g.duration >= 0) v.duration = std::max<int64_t>(1, v.frames(g.duration));
    v.ampPending = g.ampEnvs;

    v.layers.resize(g.layers.size());
    for (size_t li = 0; li < g.layers.size(); ++li) {
        const LayerData& L = g.layers[li];
        SynthLayerRun& lr = v.layers[li];
        const LayerPlan& plan = L.shape->plan;
        lr.L = &L;
        lr.shape = L.shape;
        lr.k.assign(plan.kCount, 0.0f);
        lr.ki.assign(std::max(1, plan.kiCount), 0);
        lr.words.assign(std::max(1, plan.words), 0u);
        lr.k[LayerPlan::kInvFs] = static_cast<float>(1.0 / fs);
        lr.k[LayerPlan::kGain] = L.gain >= 0 ? v.values[L.gain] : 1.0f;
        lr.offset = L.offset >= 0 ? v.frames(L.offset) : 0;
        for (const auto& [kIdx, param] : L.feeds) lr.k[kIdx] = v.values[param];

        for (const ResDef& r : L.res) {
            const NodePlan& nd = plan.nodes[r.node];
            for (size_t m = 0; m < r.modes.size(); ++m) {
                const ModeDef& md = r.modes[m];
                const double f = static_cast<double>(v.values[r.freq]) * v.values[md.ratio];
                double b = 0, a1 = 0, a2 = 0;
                if (f > 0.0 && f < 0.49 * fs) {
                    const double w = 2.0 * 3.14159265358979323846 * f / fs;
                    const double rr = std::exp(-6.907755278982137 / (static_cast<double>(v.values[md.decay]) * fs));
                    b = v.values[md.gain] * std::sin(w);   // an impulse rings at amplitude `gain`
                    a1 = 2.0 * rr * std::cos(w);
                    a2 = -rr * rr;
                }
                const int c = nd.kBase + 3 * static_cast<int>(m);
                lr.k[c] = static_cast<float>(b);
                lr.k[c + 1] = static_cast<float>(a1);
                lr.k[c + 2] = static_cast<float>(a2);
            }
        }
        lr.bufMem.resize(plan.bufCount);
        lr.bufs.assign(std::max(1, plan.bufCount), nullptr);
        for (const CombDef& c : L.combs) {
            const NodePlan& nd = plan.nodes[c.node];
            const double D = std::clamp(fs / v.values[c.freq], 2.0, 262144.0);
            const int di = static_cast<int>(std::floor(D));
            int cap = 16;
            while (cap < di + 4) cap <<= 1;
            lr.bufMem[nd.buf].assign(cap, 0.0f);
            lr.bufs[nd.buf] = lr.bufMem[nd.buf].data();
            lr.ki[nd.kiBase] = di;
            lr.ki[nd.kiBase + 1] = cap - 1;
            lr.k[nd.kBase] = static_cast<float>(D - di);
            lr.k[nd.kBase + 1] = v.values[c.feedback];
            lr.k[nd.kBase + 2] = 1.0f - v.values[c.damp];
        }
        for (size_t q = 0; q < L.rngWords.size(); ++q) {
            const uint32_t s = mix32(t.seed ^ mix32(0x51ED270Bu + static_cast<uint32_t>(li) * 977u +
                                                    static_cast<uint32_t>(q) * 131u));
            lr.words[L.rngWords[q]] = s | 1u;
        }
        lr.envs.resize(L.envs.size());
        for (size_t q = 0; q < L.envs.size(); ++q) {
            const EnvDef& d = L.envs[q];
            SynthEnvRun& e = lr.envs[q];
            const NodePlan& nd = plan.nodes[d.node];
            e.def = &d;
            e.word = nd.sBase;
            e.kBase = nd.kBase;
            e.gateAt = d.gate >= 0 ? v.frames(d.gate) : -1;
            setWordF(lr.words.data(), e.word, v.level(d.start));
            v.enter(lr, e, 0, 0);
            v.advance(lr, e, 0);
        }
    }
}

SynthVoice::~SynthVoice() = default;

bool SynthVoice::render(float* out, int n, bool compiled)
{
    if (n <= 0) return true;
    std::fill(out, out + n, 0.0f);
    SynthVoiceData& v = *d_;
    if (v.done) return true;
    FpModeGuard fpMode;
    if (releaseRequested_.exchange(false, std::memory_order_relaxed)) {
        for (SynthLayerRun& lr : v.layers)
            for (SynthEnvRun& e : lr.envs) v.release(lr, e, lr.time);
    }
    bool allCompiled = true;
    for (SynthLayerRun& lr : v.layers) allCompiled = v.runLayer(lr, out, n, compiled) && allCompiled;

    // The end: a duration, or the amplitude envelopes done and a whole tail
    // window (aligned to voice time) under the floor.
    int live = n;
    if (v.duration >= 0 && v.pos + n >= v.duration) {
        live = static_cast<int>(v.duration - v.pos);
        v.done = true;
        v.end = v.duration;
    }
    for (int i = 0; i < live;) {
        const int take = std::min(live - i, kTailWindow - v.winFill);
        float peak = v.winPeak;
        for (int j = i; j < i + take; ++j) peak = std::max(peak, std::fabs(out[j]));
        v.winPeak = peak;
        v.winFill += take;
        i += take;
        if (v.winFill == kTailWindow) {
            const int64_t w = v.pos + i;
            if (v.ampDone && v.ampDoneAt <= w - kTailWindow && v.winPeak < kTailFloor) {
                live = i;
                v.done = true;
                v.end = w;
                break;
            }
            v.winPeak = 0.0f;
            v.winFill = 0;
        }
    }
    if (live < n) std::fill(out + live, out + n, 0.0f);
    v.pos += n;
    return allCompiled;
}

bool SynthVoice::finished() const noexcept { return d_->done; }
int64_t SynthVoice::endSample() const noexcept { return d_->end; }
int64_t SynthVoice::position() const noexcept { return d_->pos; }
const std::vector<float>& SynthVoice::values() const noexcept { return d_->values; }

// --- SynthGraph ---------------------------------------------------------------

SynthGraph::SynthGraph(std::unique_ptr<SynthGraphData> d) : data_(std::move(d)) {}
SynthGraph::~SynthGraph() = default;

std::shared_ptr<const SynthGraph> SynthGraph::fromJson(std::string_view json)
{
    return std::shared_ptr<const SynthGraph>(new SynthGraph(parseSynthGraph(json)));
}

const std::vector<SynthParamInfo>& SynthGraph::params() const noexcept { return data_->params; }

int SynthGraph::paramIndex(std::string_view name) const noexcept
{
    auto it = data_->paramIndex.find(std::string(name));
    return it == data_->paramIndex.end() ? -1 : it->second;
}

int SynthGraph::layerCount() const noexcept { return static_cast<int>(data_->layers.size()); }

std::string SynthGraph::layerId(int layer) const
{
    return layer >= 0 && layer < layerCount() ? data_->layers[layer].id : std::string();
}

std::string SynthGraph::layerShapeKey(int layer) const
{
    return layer >= 0 && layer < layerCount() ? data_->layers[layer].shape->plan.key : std::string();
}

bool SynthGraph::compiled() const noexcept
{
    for (const LayerData& L : data_->layers)
        if (!L.shape->kernel()) return false;
    return true;
}

bool SynthGraph::precompile() const
{
    bool all = true;
    for (const LayerData& L : data_->layers) all = compileSynthKernelSync(*L.shape) && all;
    return all;
}

void SynthGraph::requestKernels() const
{
    for (const LayerData& L : data_->layers) requestSynthKernel(*L.shape);
}

// --- Offline --------------------------------------------------------------------

std::vector<float> renderSynth(const std::shared_ptr<const SynthGraph>& graph, const SynthRenderOptions& o)
{
    std::vector<float> out;
    if (!graph) return out;
    const bool compiled = o.compiled && graph->precompile();
    SynthVoice voice(graph, o.sampleRate, o.trigger);
    const int64_t maxFrames =
        std::max<int64_t>(0, static_cast<int64_t>(std::llround(std::max(0.0, o.maxSeconds) * o.sampleRate)));
    const int block = std::clamp(o.blockSize, 1, 1 << 16);
    std::vector<float> tmp(block);
    while (!voice.finished() && static_cast<int64_t>(out.size()) < maxFrames) {
        const int n = static_cast<int>(std::min<int64_t>(block, maxFrames - static_cast<int64_t>(out.size())));
        voice.render(tmp.data(), n, compiled);
        out.insert(out.end(), tmp.begin(), tmp.begin() + n);
    }
    if (voice.finished() && voice.endSample() >= 0 && static_cast<int64_t>(out.size()) > voice.endSample())
        out.resize(static_cast<size_t>(voice.endSample()));
    return out;
}

} // namespace broaudio
