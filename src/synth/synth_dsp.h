#pragma once

// The per-sample arithmetic of every synthesis node, written once as
// templates over an operation set `O`:
//
//   InterpOps (synth_interp.cpp)  O::F = float: the interpreter.
//   JitOps    (synth_jit.cpp)     O::F wraps a brass Value*: running the same
//                                 template emits the kernel's IR.
//
// Both instantiations build the same expression tree (C++ fixes the
// association of `a + b + c` as `(a + b) + c`, and every operator is one IEEE
// operation in both), the kernels are compiled without FMA contraction or
// reassociation, and nothing here calls a libm function: sines and tangents
// are the polynomial below, built from + - * / floor abs min max. So the
// interpreter and the kernel agree bit for bit. Float literals must carry an
// `f` suffix: a double literal would promote the interpreter's arithmetic to
// double (JitOps deletes its double overloads so such a literal fails to
// compile).
//
// A context `Cx` supplies what a node reads: cx.k(i) (float constants),
// cx.ki(i) (int constants), cx.f(w) / cx.i(w) (carried state words, by
// reference), cx.pre(node, q) (values computed once before the frame loop
// from constants) and cx.buf(b) (delay memory).

#include "synth_plan.h"

namespace broaudio::synthdsp {

// sin(2 pi a) = a (s1 + a^2 (s3 + ...)): Taylor coefficients (2 pi)^k / k!,
// accurate to ~6e-8 on the folded quarter period a in [0, 0.25].
constexpr float kS1 = 6.28318530717958648f;
constexpr float kS3 = -41.3417022403997548f;
constexpr float kS5 = 81.6052492760750372f;
constexpr float kS7 = -76.7058597530613288f;
constexpr float kS9 = 42.0586939448976611f;
constexpr float kS11 = -15.0946425768229819f;
constexpr float kInv2Pi = 0.159154943091895336f;
constexpr float kNoiseScale = 1.1920928955078125e-07f;   // 2^-23

template <class O>
typename O::F frac(typename O::F p) { return p - O::floor(p); }

// sin(2 pi p) for any p.
template <class O>
typename O::F sin01(typename O::F p)
{
    using F = typename O::F;
    F x = p - O::floor(p + 0.5f);        // [-0.5, 0.5)
    F ax = O::abs(x);
    F a = O::min(ax, 0.5f - ax);         // [0, 0.25]: sin(2 pi |x|) = sin(2 pi a)
    F a2 = a * a;
    F s = a * (kS1 + a2 * (kS3 + a2 * (kS5 + a2 * (kS7 + a2 * (kS9 + a2 * kS11)))));
    return O::sel(O::lt(x, O::c(0.0f)), -s, s);
}

// PolyBLEP residual at phase t for a per-sample increment dt (idt = 1/dt,
// omdt = 1 - dt): the band-limited step correction of a naive saw / square.
template <class O>
typename O::F blep(typename O::F t, typename O::F dt, typename O::F idt, typename O::F omdt)
{
    using F = typename O::F;
    F a = t * idt;
    F lo = a + a - a * a - 1.0f;
    F b = (t - 1.0f) * idt;
    F hi = b * b + b + b + 1.0f;
    return O::sel(O::lt(t, dt), lo, O::sel(O::lt(omdt, t), hi, O::c(0.0f)));
}

// Oscillator increment and its PolyBLEP window from a frequency.
template <class O>
void oscIncrement(typename O::F freq, typename O::F invFs, typename O::F* out)
{
    using F = typename O::F;
    F inc = freq * invFs;
    F dt = O::min(O::max(O::abs(inc), O::c(1e-6f)), O::c(0.5f));
    out[0] = inc;
    out[1] = dt;
    out[2] = 1.0f / dt;
    out[3] = 1.0f - dt;
}

// State-variable filter (TPT) coefficients: kk = 1/Q, a1, a2, a3.
template <class O>
void filterCoefs(typename O::F cutoff, typename O::F q, typename O::F invFs, typename O::F* out)
{
    using F = typename O::F;
    F w = O::min(O::max(cutoff * invFs, O::c(1e-5f)), O::c(0.49f));
    F hw = w * 0.5f;
    F g = sin01<O>(hw) / sin01<O>(hw + 0.25f);   // tan(pi w)
    F kk = 1.0f / O::max(q, O::c(0.05f));
    F a1 = 1.0f / (1.0f + g * (g + kk));
    F a2 = g * a1;
    out[0] = kk;
    out[1] = a1;
    out[2] = a2;
    out[3] = g * a2;
}

// An impulse generator's per-sample event-phase increment: rate / fs, at most
// one event every other sample.
template <class O>
typename O::F impulseIncrement(typename O::F rate, typename O::F invFs)
{
    return O::min(O::max(rate * invFs, O::c(0.0f)), O::c(0.5f));
}

template <class O, class Cx>
typename O::F slot(const SlotPlan& s, Cx& cx, const typename O::F* sig)
{
    return s.wired() ? sig[s.node] : cx.k(s.k);
}

// Values a node computes once per call from constant slots.
template <class O, class Cx>
void prepNode(const NodePlan& nd, int j, Cx& cx)
{
    using F = typename O::F;
    const F invFs = cx.k(LayerPlan::kInvFs);
    switch (nd.kind) {
    case SynthKind::Osc:
        if (!nd.slots[0].wired()) {
            F v[4];
            oscIncrement<O>(cx.k(nd.slots[0].k), invFs, v);
            for (int q = 0; q < 4; ++q) cx.pre(j, q) = v[q];
        }
        break;
    case SynthKind::Fm:
        if (!nd.slots[0].wired() && !nd.slots[1].wired()) {
            F incC = cx.k(nd.slots[0].k) * invFs;
            cx.pre(j, 0) = incC;
            cx.pre(j, 1) = incC * cx.k(nd.slots[1].k);
        }
        cx.pre(j, 2) = cx.k(nd.kBase) * kInv2Pi;
        break;
    case SynthKind::Filter:
        if (!nd.slots[1].wired() && !nd.slots[2].wired()) {
            F v[4];
            filterCoefs<O>(cx.k(nd.slots[1].k), cx.k(nd.slots[2].k), invFs, v);
            for (int q = 0; q < 4; ++q) cx.pre(j, q) = v[q];
        }
        break;
    case SynthKind::Impulses:
        if (!nd.slots[0].wired()) cx.pre(j, 0) = impulseIncrement<O>(cx.k(nd.slots[0].k), invFs);
        break;
    default:
        break;
    }
}

// One frame of node `j`; `sig` holds the outputs of nodes 0..j-1.
template <class O, class Cx>
typename O::F stepNode(const NodePlan& nd, int j, Cx& cx, const typename O::F* sig)
{
    using F = typename O::F;
    using I = typename O::I;
    const int s = nd.sBase;
    F y;
    switch (nd.kind) {
    case SynthKind::Osc: {
        F inc, dt, idt, omdt;
        if (nd.slots[0].wired()) {
            F v[4];
            oscIncrement<O>(sig[nd.slots[0].node], cx.k(LayerPlan::kInvFs), v);
            inc = v[0]; dt = v[1]; idt = v[2]; omdt = v[3];
        } else {
            inc = cx.pre(j, 0); dt = cx.pre(j, 1); idt = cx.pre(j, 2); omdt = cx.pre(j, 3);
        }
        F ph = cx.f(s);
        F p = ph + slot<O>(nd.slots[1], cx, sig);
        p = p - O::floor(p);
        switch (static_cast<OscWave>(nd.variant)) {
        case OscWave::Sine:
            y = sin01<O>(p);
            break;
        case OscWave::Saw:
            y = p + p - 1.0f - blep<O>(p, dt, idt, omdt);
            break;
        case OscWave::Square: {
            F pw = O::min(O::max(slot<O>(nd.slots[2], cx, sig), O::c(0.02f)), O::c(0.98f));
            F q = p - pw;
            q = q - O::floor(q);
            y = O::sel(O::lt(p, pw), O::c(1.0f), O::c(-1.0f)) + blep<O>(p, dt, idt, omdt) -
                blep<O>(q, dt, idt, omdt);
            break;
        }
        case OscWave::Triangle:
            y = 1.0f - 4.0f * O::abs(p - 0.5f);
            break;
        }
        F nph = ph + inc;
        cx.f(s) = nph - O::floor(nph);
        break;
    }
    case SynthKind::Fm: {
        F incC, incM;
        if (nd.slots[0].wired() || nd.slots[1].wired()) {
            incC = slot<O>(nd.slots[0], cx, sig) * cx.k(LayerPlan::kInvFs);
            incM = incC * slot<O>(nd.slots[1], cx, sig);
        } else {
            incC = cx.pre(j, 0);
            incM = cx.pre(j, 1);
        }
        F pc = cx.f(s), pm = cx.f(s + 1), prev = cx.f(s + 2);
        F m = sin01<O>(frac<O>(pm + cx.pre(j, 2) * prev));
        F cp = pc + slot<O>(nd.slots[2], cx, sig) * kInv2Pi * m;
        y = sin01<O>(frac<O>(cp));
        cx.f(s + 2) = m;
        cx.f(s) = frac<O>(pc + incC);
        cx.f(s + 1) = frac<O>(pm + incM);
        break;
    }
    case SynthKind::Noise: {
        I r = cx.i(s) * O::ic(1664525) + O::ic(1013904223);
        cx.i(s) = r;
        F w = O::itof(O::ashr8(r)) * kNoiseScale;
        switch (static_cast<NoiseColor>(nd.variant)) {
        case NoiseColor::White:
            y = w;
            break;
        case NoiseColor::Pink: {
            // Paul Kellet's pink filter.
            F b0 = 0.99886f * cx.f(s + 1) + w * 0.0555179f;
            F b1 = 0.99332f * cx.f(s + 2) + w * 0.0750759f;
            F b2 = 0.96900f * cx.f(s + 3) + w * 0.1538520f;
            F b3 = 0.86650f * cx.f(s + 4) + w * 0.3104856f;
            F b4 = 0.55000f * cx.f(s + 5) + w * 0.5329522f;
            F b5 = -0.7616f * cx.f(s + 6) - w * 0.0168980f;
            F pink = b0 + b1 + b2 + b3 + b4 + b5 + cx.f(s + 7) + w * 0.5362f;
            cx.f(s + 1) = b0; cx.f(s + 2) = b1; cx.f(s + 3) = b2;
            cx.f(s + 4) = b3; cx.f(s + 5) = b4; cx.f(s + 6) = b5;
            cx.f(s + 7) = w * 0.115926f;
            y = pink * 0.11f;
            break;
        }
        case NoiseColor::Brown: {
            F z = cx.f(s + 1) * 0.998f + w * 0.02f;
            cx.f(s + 1) = z;
            y = z * 2.5f;
            break;
        }
        }
        break;
    }
    case SynthKind::Filter: {
        F kk, a1, a2, a3;
        if (nd.slots[1].wired() || nd.slots[2].wired()) {
            F v[4];
            filterCoefs<O>(slot<O>(nd.slots[1], cx, sig), slot<O>(nd.slots[2], cx, sig),
                           cx.k(LayerPlan::kInvFs), v);
            kk = v[0]; a1 = v[1]; a2 = v[2]; a3 = v[3];
        } else {
            kk = cx.pre(j, 0); a1 = cx.pre(j, 1); a2 = cx.pre(j, 2); a3 = cx.pre(j, 3);
        }
        F x = slot<O>(nd.slots[0], cx, sig);
        F ic1 = cx.f(s), ic2 = cx.f(s + 1);
        F v3 = x - ic2;
        F v1 = a1 * ic1 + a2 * v3;
        F v2 = ic2 + a2 * ic1 + a3 * v3;
        cx.f(s) = v1 + v1 - ic1;
        cx.f(s + 1) = v2 + v2 - ic2;
        switch (static_cast<FilterMode>(nd.variant)) {
        case FilterMode::Lowpass: y = v2; break;
        case FilterMode::Highpass: y = x - kk * v1 - v2; break;
        case FilterMode::Bandpass: y = kk * v1; break;
        case FilterMode::Notch: y = x - kk * v1; break;
        }
        break;
    }
    case SynthKind::Env: {
        F e = cx.f(s);
        cx.f(s) = e * cx.k(nd.kBase) + cx.k(nd.kBase + 1);
        y = e;
        break;
    }
    case SynthKind::Shaper: {
        F x = slot<O>(nd.slots[0], cx, sig) * slot<O>(nd.slots[1], cx, sig);
        switch (static_cast<ShaperMode>(nd.variant)) {
        case ShaperMode::Tanh: {
            x = O::min(O::max(x, O::c(-3.0f)), O::c(3.0f));
            F x2 = x * x;
            y = x * (x2 + 27.0f) / (x2 * 9.0f + 27.0f);
            break;
        }
        case ShaperMode::Clip:
            y = O::min(O::max(x, O::c(-1.0f)), O::c(1.0f));
            break;
        case ShaperMode::Fold: {
            F t = x * 0.25f + 0.25f;
            t = t - O::floor(t);
            y = 1.0f - 4.0f * O::abs(t - 0.5f);
            break;
        }
        }
        break;
    }
    case SynthKind::Resonator: {
        F x = slot<O>(nd.slots[0], cx, sig);
        for (int m = 0; m < nd.count; ++m) {
            const int c = nd.kBase + 3 * m;
            F y1 = cx.f(s + 2 * m), y2 = cx.f(s + 2 * m + 1);
            F v = cx.k(c) * x + cx.k(c + 1) * y1 + cx.k(c + 2) * y2;
            cx.f(s + 2 * m + 1) = y1;
            cx.f(s + 2 * m) = v;
            y = m == 0 ? v : y + v;
        }
        break;
    }
    case SynthKind::Comb: {
        F x = slot<O>(nd.slots[0], cx, sig);
        I w = cx.i(s);
        I mask = cx.ki(nd.kiBase + 1);
        I rp = w - cx.ki(nd.kiBase);
        auto buf = cx.buf(nd.buf);
        F d0 = O::load(buf, rp & mask);
        F d1 = O::load(buf, (rp - O::ic(1)) & mask);
        F d = d0 + cx.k(nd.kBase) * (d1 - d0);
        F lp = cx.f(s + 1);
        lp = lp + cx.k(nd.kBase + 2) * (d - lp);
        cx.f(s + 1) = lp;
        y = x + cx.k(nd.kBase + 1) * lp;
        O::store(buf, w, y);
        cx.i(s) = (w + O::ic(1)) & mask;
        break;
    }
    case SynthKind::Impulses: {
        // Stochastic events: an event phase advances by rate / fs and an event
        // fires when it reaches the threshold, which each event redraws as
        // 1 + jitter * u (u uniform in [-1, 1), floored at 0.05), so the mean
        // interval stays 1 / rate. Each event draws its amplitude
        // 1 - ampJitter * u' (u' in [0, 1)) and restarts the grain. The rng
        // steps twice every frame whether or not an event fires, so the
        // stream depends only on the frame count. Words start at 0: the first
        // event fires on the first frame.
        const F inc = nd.slots[0].wired() ? impulseIncrement<O>(sig[nd.slots[0].node], cx.k(LayerPlan::kInvFs))
                                          : cx.pre(j, 0);
        I r1 = cx.i(s) * O::ic(1664525) + O::ic(1013904223);
        I r2 = r1 * O::ic(1664525) + O::ic(1013904223);
        cx.i(s) = r2;
        F u1 = O::itof(O::ashr8(r1)) * kNoiseScale;
        F u2 = O::itof(O::ashr8(r2)) * kNoiseScale;
        F ph = cx.f(s + 1) + inc;
        F thr = cx.f(s + 2);
        auto wait = O::lt(ph, thr);   // no event this frame
        F nthr = O::max(1.0f + cx.k(nd.kBase) * u1, O::c(0.05f));
        cx.f(s + 1) = O::sel(wait, ph, ph - thr);
        cx.f(s + 2) = O::sel(wait, thr, nthr);
        F na = 1.0f - cx.k(nd.kBase + 1) * (u2 * 0.5f + 0.5f);
        F a = O::sel(wait, cx.f(s + 4), na);
        F pos = O::sel(wait, cx.f(s + 3) + cx.k(nd.kBase + 2), O::c(0.0f));
        cx.f(s + 3) = pos;
        cx.f(s + 4) = a;
        switch (static_cast<ImpulseShape>(nd.variant)) {
        case ImpulseShape::Impulse:
            y = O::sel(wait, O::c(0.0f), a);
            break;
        case ImpulseShape::Rect:
            y = O::sel(O::lt(pos, O::c(1.0f)), a, O::c(0.0f));
            break;
        case ImpulseShape::Hann: {
            F h = sin01<O>(pos * 0.5f);   // sin(pi pos)
            y = O::sel(O::lt(pos, O::c(1.0f)), a * (h * h), O::c(0.0f));
            break;
        }
        case ImpulseShape::Decay: {
            F e = O::sel(wait, cx.f(s + 5) * cx.k(nd.kBase + 3), a);
            cx.f(s + 5) = e;
            y = e;
            break;
        }
        }
        break;
    }
    case SynthKind::Mul:
        y = slot<O>(nd.slots[0], cx, sig) * slot<O>(nd.slots[1], cx, sig);
        break;
    case SynthKind::Mix:
        for (int q = 0; q < nd.count; ++q) {
            F v = slot<O>(nd.slots[q], cx, sig) * cx.k(nd.kBase + q);
            y = q == 0 ? v : y + v;
        }
        break;
    }
    return y * slot<O>(nd.gain(), cx, sig);
}

// One frame of a layer: its output node's value.
template <class O, class Cx>
typename O::F layerFrame(const LayerPlan& plan, Cx& cx)
{
    typename O::F sig[kSynthMaxNodes];
    for (size_t j = 0; j < plan.nodes.size(); ++j)
        sig[j] = stepNode<O>(plan.nodes[j], static_cast<int>(j), cx, sig);
    return sig[plan.output];
}

} // namespace broaudio::synthdsp
