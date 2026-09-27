// Seamless loops (SynthLoopOptions, synth/synth_graph.h): the loop is the
// voice itself across the seam (its last sample and its first are two
// consecutive voice samples, so filter, resonator and comb state carry over)
// and no click is measurable there; snapping puts periodic rates on whole
// cycles, so a snapped tone tiles as one continuous waveform; compiled and
// interpreted loops agree bit for bit; a looping voice plays the offline
// loop sample for sample, whatever its block sizes, then crossfades into
// its release tail and ends; renders are deterministic across seeds and
// threads.

#include "test_harness.h"
#include "broaudio/spatial/voice_jit.h"
#include "broaudio/synth/synth_graph.h"

#include <algorithm>
#include <cstring>
#include <numbers>
#include <thread>

using namespace broaudio;

namespace {

constexpr int kSr = 48000;

// A drone with no end: a saw through a lowpass whose cutoff an LFO moves,
// an FM partial, highpassed noise ringing a resonator, and resonant ticks.
const char* kHum = R"({"layers": {
  "tone": {"gain": 0.3, "nodes": {
      "lfo": {"type": "osc", "wave": "sine", "freq": 0.3, "gain": 150},
      "cut": {"type": "mix", "inputs": [600, "lfo"]},
      "s":   {"type": "osc", "wave": "saw", "freq": 55.3},
      "lp":  {"type": "filter", "mode": "lowpass", "input": "s", "cutoff": "cut", "q": 3},
      "fm":  {"type": "fm", "freq": 110.2, "ratio": 1.4142, "index": 1.5, "gain": 0.2},
      "m":   {"type": "mix", "inputs": ["lp", "fm"]}
    }, "output": "m"},
  "hiss": {"gain": 0.5, "nodes": {
      "n":   {"type": "noise"},
      "hp":  {"type": "filter", "mode": "highpass", "input": "n", "cutoff": 2500, "q": 0.7},
      "res": {"type": "resonator", "input": "hp", "freq": 700, "modes": [{"ratio": 1, "decay": 0.4, "gain": 0.02}]}
    }, "output": "res"},
  "tick": {"nodes": {
      "t":   {"type": "impulses", "shape": "decay", "rate": 7.3, "length": 0.004},
      "rr":  {"type": "resonator", "input": "t", "freq": 1500, "modes": [{"ratio": 1, "decay": 0.08, "gain": 0.3}]}
    }, "output": "rr"}
}})";

// A held note with a release.
const char* kHeld = R"({"nodes": {
    "o":  {"type": "osc", "wave": "saw", "freq": 110, "gain": "e"},
    "e":  {"type": "env", "attack": 0.01, "decay": 0.05, "sustain": 0.6, "release": 0.3},
    "lp": {"type": "filter", "input": "o", "cutoff": 1200}
  }, "output": "lp"})";

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

bool sameBit(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

SynthLoopOptions loopOf(double length, double crossfade, double start, bool snap,
                        SynthLoopCurve curve = SynthLoopCurve::Auto)
{
    SynthLoopOptions l;
    l.length = length;
    l.crossfade = crossfade;
    l.start = start;
    l.snap = snap;
    l.curve = curve;
    return l;
}

std::vector<float> renderLoop(const std::shared_ptr<const SynthGraph>& g, const SynthLoopOptions& l,
                              uint32_t seed = 1, bool compiled = true)
{
    SynthRenderOptions o;
    o.sampleRate = kSr;
    o.trigger.seed = seed;
    o.compiled = compiled;
    o.loop = l;
    return renderSynth(g, o);
}

// The plain voice, frames [0, n).
std::vector<float> plain(const std::shared_ptr<const SynthGraph>& g, int n, uint32_t seed = 1)
{
    SynthTrigger t;
    t.seed = seed;
    SynthVoice v(g, kSr, t);
    std::vector<float> out(n);
    for (int p = 0; p < n; p += 1000) v.render(out.data() + p, std::min(1000, n - p), false);
    return out;
}

// |second difference| at i: what a click shows up in.
double curvature(const std::vector<float>& x, size_t i)
{
    return std::fabs(static_cast<double>(x[i + 1]) - 2.0 * x[i] + x[i - 1]);
}

} // namespace

TEST(the_seam_is_the_voice_itself)
{
    auto g = SynthGraph::fromJson(kHum);
    const int len = kSr / 2, fade = kSr / 20, start = kSr / 4;
    const auto y = plain(g, start + len + fade);
    for (SynthLoopCurve curve : {SynthLoopCurve::Auto, SynthLoopCurve::Power, SynthLoopCurve::Linear}) {
        const auto L = renderLoop(g, loopOf(0.5, 0.05, 0.25, false, curve));
        ASSERT_EQ(static_cast<int>(L.size()), len);
        // Past the crossfade the loop is the voice; its first sample is the
        // voice's sample after the loop's last one.
        for (int i = fade; i < len; i++) ASSERT_TRUE(sameBit(L[i], y[start + i]));
        ASSERT_TRUE(sameBit(L[0], y[start + len]));
        ASSERT_TRUE(sameBit(L[len - 1], y[start + len - 1]));
        // Inside the crossfade it is a blend of the two, never louder than
        // their sum.
        for (int i = 1; i < fade; i++)
            ASSERT_TRUE(std::fabs(L[i]) <= std::fabs(y[start + i]) + std::fabs(y[start + len + i]) + 1e-6f);

        // No click: tiled three times, the curvature at the two seams is no
        // larger than anywhere inside the loop.
        std::vector<float> tiled;
        for (int k = 0; k < 3; k++) tiled.insert(tiled.end(), L.begin(), L.end());
        double inside = 0, seam = 0;
        for (size_t i = 1; i + 1 < tiled.size(); i++) {
            const bool atSeam = (i % len) <= 1 || (i % len) >= static_cast<size_t>(len - 1);
            (atSeam ? seam : inside) = std::max(atSeam ? seam : inside, curvature(tiled, i));
        }
        ASSERT_TRUE(seam <= inside);
    }
    // Without a crossfade the loop is the voice's frames exactly (a hard
    // loop: the seam is not continuous).
    const auto hard = renderLoop(g, loopOf(0.5, 0.0, 0.25, false));
    for (int i = 0; i < len; i++) ASSERT_TRUE(sameBit(hard[i], y[start + i]));
    PASS();
}

TEST(snap_puts_periodic_rates_on_whole_cycles)
{
    auto g = SynthGraph::fromJson(kHum);
    SynthVoice v(g, kSr, SynthTrigger{}, loopOf(0.5, 0.05, 0.25, true));
    const auto& val = v.values();
    ASSERT_TRUE(val[g->paramIndex("s.freq")] == 56.0f);     // 27.65 cycles -> 28
    ASSERT_TRUE(val[g->paramIndex("lfo.freq")] == 2.0f);    // 0.15 -> at least one cycle
    ASSERT_TRUE(val[g->paramIndex("fm.freq")] == 110.0f);   // 55.1 -> 55
    ASSERT_NEAR(val[g->paramIndex("fm.ratio")], 156.0f / 110.0f, 1e-6f);   // modulator 77.8 -> 78 cycles
    ASSERT_TRUE(val[g->paramIndex("t.rate")] == 8.0f);      // 3.65 -> 4
    // Not a periodic rate: untouched.
    ASSERT_TRUE(val[g->paramIndex("hp.cutoff")] == 2500.0f);
    SynthVoice unsnapped(g, kSr, SynthTrigger{}, loopOf(0.5, 0.05, 0.25, false));
    ASSERT_TRUE(unsnapped.values()[g->paramIndex("s.freq")] == 55.3f);

    // A snapped sine tiles as one continuous sine, crossfade included.
    auto sine = SynthGraph::fromJson(R"({"nodes": {"o": {"type": "osc", "freq": 441}}, "output": "o"})");
    const auto L = renderLoop(sine, loopOf(0.5, 0.05, 0.25, true));
    const int start = kSr / 4;
    double worst = 0;
    for (size_t i = 0; i < L.size(); i++) {
        const double ideal = std::sin(2.0 * std::numbers::pi * 442.0 * static_cast<double>(start + i) / kSr);
        worst = std::max(worst, std::fabs(L[i] - ideal));
    }
    ASSERT_LT(worst, 2e-3);
    PASS();
}

TEST(snap_folds_constant_mixes_and_round_trips)
{
    // o1's frequency is a mix of numbers (55.3 Hz), o2's a mul of that mix
    // by 2, the FM carrier a mix too; the vibrato osc's is a real signal.
    auto g = SynthGraph::fromJson(R"({"nodes": {
        "f":   {"type": "mix", "inputs": [40, {"node": "c", "weight": 1}]},
        "c":   {"type": "mix", "inputs": [15.3]},
        "o1":  {"type": "osc", "wave": "saw", "freq": "f"},
        "f2":  {"type": "mul", "a": "f", "b": 2},
        "o2":  {"type": "osc", "freq": "f2", "gain": 0.3},
        "fc":  {"type": "mix", "inputs": [100, 10.2]},
        "fm":  {"type": "fm", "freq": "fc", "ratio": 1.4142, "gain": 0.2},
        "lfo": {"type": "osc", "freq": 3, "gain": 4},
        "vf":  {"type": "mix", "inputs": [220, "lfo"]},
        "vib": {"type": "osc", "freq": "vf", "gain": 0.1},
        "m":   {"type": "mix", "inputs": ["o1", "o2", "fm", "vib"]}
      }, "output": "m"})");
    const SynthLoopOptions l = loopOf(0.5, 0.05, 0.25, true);
    SynthTrigger t;
    t.jitter = false;
    const std::vector<float> v = g->values(t, kSr, l);
    auto at = [&](const char* n) { return v[g->paramIndex(n)]; };
    // 55.3 -> 56 Hz: the mix's first input carries it (40 -> 40.7).
    ASSERT_NEAR(at("f.inputs.0") + at("c.inputs.0"), 56.0f, 1e-4f);
    ASSERT_TRUE(at("c.inputs.0") == 15.3f);
    // 2 x 56 = 112 is already whole: the mul's factor stays.
    ASSERT_TRUE(at("f2.b") == 2.0f);
    // The carrier 110.2 -> 110, then the ratio follows (155.6 -> 156 Hz).
    ASSERT_NEAR(at("fc.inputs.0") + at("fc.inputs.1"), 110.0f, 1e-4f);
    ASSERT_NEAR(at("fm.ratio"), 156.0f / 110.0f, 1e-5f);
    // A signal frequency is not constant: untouched (the LFO itself snaps).
    ASSERT_TRUE(at("vf.inputs.0") == 220.0f);
    ASSERT_TRUE(at("lfo.freq") == 4.0f);
    // What the looping voice reports.
    SynthVoice voice(g, kSr, t, l);
    ASSERT_TRUE(voice.values() == v);
    // Without a loop (or without snap) nothing moves.
    ASSERT_TRUE(g->values(t, kSr)[g->paramIndex("f.inputs.0")] == 40.0f);

    // Idempotent: the snapped values as overrides resolve to themselves and
    // render the same loop.
    SynthTrigger back = t;
    for (size_t i = 0; i < v.size(); i++) back.overrides.push_back({static_cast<int>(i), v[i]});
    ASSERT_TRUE(g->values(back, kSr, l) == v);
    SynthRenderOptions o;
    o.trigger = t;
    o.loop = l;
    const auto a = renderSynth(g, o);
    o.trigger = back;
    ASSERT_TRUE(sameBits(renderSynth(g, o), a));
    PASS();
}

TEST(each_layer_crossfades_with_its_own_curve)
{
    // A snapped hum (halves identical) and a hiss (halves unrelated), at
    // equal power. As two layers each blends with its own curve: the hum
    // and the hiss each keep their level through the crossfade. Mixed
    // inside one layer one curve serves both (the power-weighted
    // correlation, 0.5): the total stays flat but the hum lifts and the
    // hiss dips mid-crossfade.
    const char* layered = R"({"layers": {
        "hum":  {"nodes": {"o": {"type": "osc", "freq": 440, "gain": 0.5}}, "output": "o"},
        "hiss": {"nodes": {"n": {"type": "noise"},
                           "hp": {"type": "filter", "mode": "highpass", "input": "n", "cutoff": 4000, "gain": 0.65}},
                 "output": "hp"}}})";
    const char* single = R"({"nodes": {
        "o": {"type": "osc", "freq": 440, "gain": 0.5}, "n": {"type": "noise"},
        "hp": {"type": "filter", "mode": "highpass", "input": "n", "cutoff": 4000, "gain": 0.65},
        "m": {"type": "mix", "inputs": ["o", "hp"]}}, "output": "m"})";
    const int len = kSr, fade = kSr / 5, win = fade * 3 / 10, start = kSr / 4;
    // Over a window: the 440 Hz component's power (least squares on sin and
    // cos) and the rest's (the hiss, far above it).
    struct Parts { double hum, hiss, total; };
    auto parts = [&](const std::vector<float>& x, int from) {
        double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0, xx = 0;
        for (int i = from; i < from + win; i++) {
            const double ph = 2.0 * std::numbers::pi * 440.0 * (start + i) / kSr;
            const double s = std::sin(ph), c = std::cos(ph);
            ss += s * s; cc += c * c; sc += s * c;
            xs += x[i] * s; xc += x[i] * c; xx += static_cast<double>(x[i]) * x[i];
        }
        const double det = ss * cc - sc * sc;
        const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
        const double hum = (a * a + b * b) / 2.0;
        return Parts{hum, xx / win - hum, xx / win};
    };
    auto lifts = [&](const char* desc) {
        const auto L = renderLoop(SynthGraph::fromJson(desc), loopOf(1.0, 0.2, 0.25, true), 3);
        Parts steady{0, 0, 0};
        for (int k = 0; k < 6; k++) {
            const Parts p = parts(L, fade + k * (len - fade - win) / 6);
            steady.hum += p.hum / 6; steady.hiss += p.hiss / 6; steady.total += p.total / 6;
        }
        const Parts mid = parts(L, fade / 2 - win / 2);
        return Parts{10 * std::log10(mid.hum / steady.hum), 10 * std::log10(mid.hiss / steady.hiss),
                     10 * std::log10(mid.total / steady.total)};
    };
    const Parts per = lifts(layered), one = lifts(single);
    std::printf("  mid-crossfade vs steady, dB: per layer hum %+.2f hiss %+.2f total %+.2f; "
                "one layer hum %+.2f hiss %+.2f total %+.2f\n",
                per.hum, per.hiss, per.total, one.hum, one.hiss, one.total);
    ASSERT_LT(std::fabs(per.hum), 0.05);
    ASSERT_LT(std::fabs(per.hiss), 0.3);
    ASSERT_LT(std::fabs(per.total), 0.2);
    ASSERT_GT(one.hum, 0.8);
    ASSERT_LT(one.hiss, -0.8);
    PASS();
}

TEST(compiled_loops_match_interpreted)
{
    if (!voiceJitBackendAvailable()) {
        std::printf("  (no brass backend: skipped)\n");
        PASS();
        return;
    }
    for (const char* desc : {kHum, kHeld}) {
        auto g = SynthGraph::fromJson(desc);
        ASSERT_TRUE(g->precompile());
        for (uint32_t seed : {1u, 5u}) {
            const SynthLoopOptions l = loopOf(0.4, 0.06, 0.3, true);
            ASSERT_TRUE(sameBits(renderLoop(g, l, seed, true), renderLoop(g, l, seed, false)));
            SynthVoice a(g, kSr, SynthTrigger{seed}, l, true), b(g, kSr, SynthTrigger{seed}, l, false);
            ASSERT_TRUE(sameBits(a.releaseTail(), b.releaseTail()));
        }
    }
    PASS();
}

TEST(a_looping_voice_plays_the_offline_loop_then_releases)
{
    auto g = SynthGraph::fromJson(kHeld);
    const SynthLoopOptions l = loopOf(0.25, 0.03, 0.2, true);
    const auto L = renderLoop(g, l, 3);
    ASSERT_EQ(static_cast<int>(L.size()), kSr / 4);
    SynthTrigger t;
    t.seed = 3;
    SynthVoice v(g, kSr, t, l);
    ASSERT_TRUE(v.looping());
    ASSERT_TRUE(sameBits(v.loopSamples(), L));
    const int sizes[] = {1, 37, 500, 4096, 128, 999};
    std::vector<float> out, tmp(4096);
    for (int b = 0; static_cast<int>(out.size()) < 7 * static_cast<int>(L.size()) / 2; b++) {
        const int n = sizes[b % 6];
        ASSERT_TRUE(v.render(tmp.data(), n, true));
        out.insert(out.end(), tmp.begin(), tmp.begin() + n);
    }
    for (size_t i = 0; i < out.size(); i++) ASSERT_TRUE(sameBit(out[i], L[i % L.size()]));
    ASSERT_FALSE(v.finished());
    ASSERT_EQ(v.position(), static_cast<int64_t>(out.size()));

    // Note-off: the release tail (the voice released at the seam) takes
    // over, decays, and the voice ends.
    const auto& tail = v.releaseTail();
    ASSERT_GT(static_cast<int>(tail.size()), kSr / 5);
    ASSERT_TRUE(tail.size() < static_cast<size_t>(kSr));
    v.release();
    std::vector<float> rel;
    while (!v.finished() && rel.size() < static_cast<size_t>(4 * kSr)) {
        v.render(tmp.data(), 512, true);
        rel.insert(rel.end(), tmp.begin(), tmp.begin() + 512);
    }
    ASSERT_TRUE(v.finished());
    ASSERT_EQ(v.endSample(), static_cast<int64_t>(out.size() + tail.size()));
    const size_t fade = static_cast<size_t>(0.02 * kSr);
    for (size_t k = fade; k < tail.size(); k++) ASSERT_TRUE(sameBit(rel[k], tail[k]));
    for (size_t k = tail.size(); k < rel.size(); k++) ASSERT_TRUE(rel[k] == 0.0f);
    // The first release sample continues the loop where it was.
    ASSERT_LT(std::fabs(rel[0] - L[out.size() % L.size()]), 0.05f);

    // No amplitude envelope, nothing to release: the loop fades out.
    auto hum = SynthGraph::fromJson(kHum);
    SynthVoice h(hum, kSr, SynthTrigger{}, loopOf(0.2, 0.02, 0.1, true));
    ASSERT_TRUE(h.releaseTail().empty());
    h.render(tmp.data(), 1000, true);
    h.release();
    h.render(tmp.data(), 4096, true);
    ASSERT_TRUE(h.finished());
    ASSERT_EQ(h.endSample(), static_cast<int64_t>(1000 + fade));
    PASS();
}

TEST(loops_ignore_the_duration_and_are_deterministic)
{
    // A duration ends the one-shot, not the loop.
    auto capped = SynthGraph::fromJson(R"({"nodes": {"o": {"type": "osc", "freq": 200}}, "output": "o", "duration": 0.1})");
    const auto L = renderLoop(capped, loopOf(0.5, 0.05, 0.25, true));
    ASSERT_EQ(static_cast<int>(L.size()), kSr / 2);
    float tailPeak = 0;
    for (size_t i = L.size() - 4800; i < L.size(); i++) tailPeak = std::max(tailPeak, std::fabs(L[i]));
    ASSERT_GT(tailPeak, 0.9f);

    auto g = SynthGraph::fromJson(kHum);
    const SynthLoopOptions l = loopOf(0.3, 0.05, 0.2, true);
    const auto ref = renderLoop(g, l, 11);
    ASSERT_FALSE(sameBits(ref, renderLoop(g, l, 12)));
    std::vector<std::vector<float>> outs(4);
    std::vector<std::thread> ts;
    for (int k = 0; k < 4; k++) ts.emplace_back([&, k] { outs[k] = renderLoop(g, l, 11, k % 2 == 0); });
    for (auto& t : ts) t.join();
    for (const auto& o : outs) ASSERT_TRUE(sameBits(o, ref));
    PASS();
}

int main() { return runAllTests(); }
