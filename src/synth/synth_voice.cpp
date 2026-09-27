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

#include "synth_voice_data.h"

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

int64_t SynthVoiceData::frames(int param) const
{
    return static_cast<int64_t>(std::llround(static_cast<double>(std::max(0.0f, values[param])) * fs));
}

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

// Whole cycles over the loop for every constant periodic rate: oscillator
// and FM carrier frequencies (the ratio following, so the modulator is whole
// too) and impulse rates. At least one cycle; clamped into the range.
void SynthVoiceData::snap(const SynthGraphData& g, int64_t loopFrames)
{
    const double perCycle = static_cast<double>(fs) / static_cast<double>(loopFrames);   // Hz of one cycle
    auto cyclesOf = [&](double hz) { return std::max<double>(1.0, std::round(hz / perCycle)); };
    auto snapParam = [&](int p) -> double {
        if (p < 0 || !(values[p] > 0.0f)) return 0.0;
        const SynthParamInfo& info = g.params[p];
        values[p] = std::clamp(static_cast<float>(cyclesOf(values[p]) * perCycle), info.lo, info.hi);
        return values[p];
    };
    for (const LayerData& L : g.layers) {
        const LayerPlan& plan = L.shape->plan;
        std::vector<int> paramOfK(plan.kCount, -1);
        for (const auto& [kIdx, param] : L.feeds) paramOfK[kIdx] = param;
        for (const NodePlan& nd : plan.nodes) {
            const bool constFreq = !nd.slots.empty() && !nd.slots[0].wired();
            if (nd.kind == SynthKind::Osc || nd.kind == SynthKind::Impulses) {
                if (constFreq) snapParam(paramOfK[nd.slots[0].k]);
            } else if (nd.kind == SynthKind::Fm && constFreq) {
                const double fc = snapParam(paramOfK[nd.slots[0].k]);
                const int pr = nd.slots[1].wired() ? -1 : paramOfK[nd.slots[1].k];
                if (fc > 0.0 && pr >= 0 && values[pr] > 0.0f) {
                    const double fm = cyclesOf(fc * values[pr]) * perCycle;
                    const SynthParamInfo& info = g.params[pr];
                    values[pr] = std::clamp(static_cast<float>(fm / fc), info.lo, info.hi);
                }
            }
        }
    }
}

void SynthVoiceData::init(const SynthGraphData& g, int sampleRate, const SynthTrigger& t, int64_t snapFrames)
{
    SynthVoiceData& v = *this;
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
    if (snapFrames > 0) v.snap(g, snapFrames);
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
        for (size_t q = 0; q < L.impulses.size(); ++q) {
            const ImpDef& d = L.impulses[q];
            const NodePlan& nd = plan.nodes[d.node];
            const double len = std::max(1.0, static_cast<double>(v.values[d.length]) * fs);   // frames
            lr.k[nd.kBase + 2] = static_cast<float>(1.0 / len);
            lr.k[nd.kBase + 3] = static_cast<float>(std::exp(-6.907755278982137 / len));
            const uint32_t s = mix32(t.seed ^ mix32(0xA511E9B3u + static_cast<uint32_t>(li) * 977u +
                                                    static_cast<uint32_t>(q) * 131u));
            lr.words[nd.sBase] = s | 1u;
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

void SynthVoiceData::releaseAll()
{
    for (SynthLayerRun& lr : layers)
        for (SynthEnvRun& e : lr.envs) release(lr, e, lr.time);
}

std::unique_ptr<SynthVoiceData> SynthVoiceData::clone() const
{
    auto c = std::make_unique<SynthVoiceData>(*this);
    for (SynthLayerRun& lr : c->layers)
        for (size_t b = 0; b < lr.bufMem.size(); ++b)
            if (!lr.bufMem[b].empty()) lr.bufs[b] = lr.bufMem[b].data();
    return c;
}

bool SynthVoiceData::render(float* out, int n, bool compiled)
{
    if (n <= 0) return true;
    std::fill(out, out + n, 0.0f);
    SynthVoiceData& v = *this;
    if (v.done) return true;
    FpModeGuard fpMode;
    bool allCompiled = true;
    for (SynthLayerRun& lr : v.layers) allCompiled = v.runLayer(lr, out, n, compiled) && allCompiled;
    if (v.endless) {
        v.pos += n;
        return allCompiled;
    }

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

// --- SynthVoice ---------------------------------------------------------------

SynthVoice::SynthVoice(std::shared_ptr<const SynthGraph> graph, int sampleRate, const SynthTrigger& t)
    : graph_(std::move(graph)), d_(std::make_unique<SynthVoiceData>())
{
    d_->init(graph_->data(), sampleRate, t, 0);
}

SynthVoice::SynthVoice(std::shared_ptr<const SynthGraph> graph, int sampleRate, const SynthTrigger& t,
                       const SynthLoopOptions& loop, bool compiled)
    : graph_(std::move(graph)), d_(std::make_unique<SynthVoiceData>())
{
    if (!loop.enabled()) {
        d_->init(graph_->data(), sampleRate, t, 0);
        return;
    }
    const int fs = sampleRate > 0 ? sampleRate : 48000;
    d_->init(graph_->data(), fs, t, loop.snap ? synthLoopFrames(loop, fs) : 0);
    loop_ = buildSynthLoop(*d_, graph_->data(), loop, compiled);
    // Only the values outlive the cut: the loop plays from memory.
    d_->layers.clear();
    d_->layers.shrink_to_fit();
}

SynthVoice::~SynthVoice() = default;

bool SynthVoice::render(float* out, int n, bool compiled)
{
    if (loop_) return renderLoop(out, n);
    if (n > 0 && !d_->done && releaseRequested_.exchange(false, std::memory_order_relaxed)) {
        FpModeGuard fpMode;
        d_->releaseAll();
    }
    return d_->render(out, n, compiled);
}

bool SynthVoice::renderLoop(float* out, int n)
{
    if (n <= 0) return true;
    SynthLoopData& L = *loop_;
    if (L.done) {
        std::fill(out, out + n, 0.0f);
        L.played += n;
        return true;
    }
    if (releaseRequested_.exchange(false, std::memory_order_relaxed) && L.rel < 0) {
        L.rel = 0;
        L.relFrom = L.pos;
    }
    const int64_t len = static_cast<int64_t>(L.loop.size());
    const int64_t fade = static_cast<int64_t>(L.fadeIn.size());
    const int64_t relEnd = std::max(fade, static_cast<int64_t>(L.tail.size()));
    int i = 0;
    for (; i < n; ++i) {
        if (L.rel < 0) {
            out[i] = L.loop[L.pos];
            if (++L.pos == len) L.pos = 0;
            continue;
        }
        if (L.rel >= relEnd) {
            L.done = true;
            L.end = L.played + i;
            break;
        }
        const int64_t k = L.rel++;
        float s = k < static_cast<int64_t>(L.tail.size()) ? L.tail[k] : 0.0f;
        if (k < fade) s = L.loop[(L.relFrom + k) % len] * L.fadeOut[k] + s * L.fadeIn[k];
        out[i] = s;
    }
    if (!L.done && L.rel >= relEnd) {
        L.done = true;
        L.end = L.played + i;
    }
    std::fill(out + i, out + n, 0.0f);
    L.played += n;
    return true;
}

bool SynthVoice::finished() const noexcept { return loop_ ? loop_->done : d_->done; }
int64_t SynthVoice::endSample() const noexcept { return loop_ ? loop_->end : d_->end; }
int64_t SynthVoice::position() const noexcept { return loop_ ? loop_->played : d_->pos; }
const std::vector<float>& SynthVoice::values() const noexcept { return d_->values; }

const std::vector<float>& SynthVoice::loopSamples() const noexcept
{
    static const std::vector<float> kEmpty;
    return loop_ ? loop_->loop : kEmpty;
}

const std::vector<float>& SynthVoice::releaseTail() const noexcept
{
    static const std::vector<float> kEmpty;
    return loop_ ? loop_->tail : kEmpty;
}

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
    if (o.loop.enabled()) {
        SynthVoice voice(graph, o.sampleRate, o.trigger, o.loop, compiled);
        return voice.loopSamples();
    }
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
