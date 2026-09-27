// ear::fit (ear/fit.h): recovering hidden parameters of a synthesis graph
// from a reference render, meeting measurement and band targets, bit-for-bit
// determinism across thread counts, held parameters, the external scorer's
// batches and thread, jitter averaging over seeds, cancellation, the time
// budget and the option errors.

#include "test_harness.h"
#include "broaudio/ear/fit.h"
#include "broaudio/synth/synth_graph.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

using namespace broaudio;
namespace ear = broaudio::ear;

namespace {

// An FM pluck: the index decays faster than the level.
const char* kPluck = R"({"nodes": {
    "o":  {"type": "fm", "freq": 440, "ratio": 2, "index": "ie", "gain": "ae"},
    "ie": {"type": "env", "attack": 0.001, "decay": 0.15, "sustain": 0, "release": 0.05, "peak": 3},
    "ae": {"type": "env", "attack": 0.002, "decay": 0.3, "sustain": 0, "release": 0.05}
  }, "output": "o", "duration": 0.8})";

// Filtered noise with a jittered cutoff.
const char* kHiss = R"({"nodes": {
    "n":  {"type": "noise", "color": "white", "gain": "e"},
    "e":  {"type": "env", "attack": 0.002, "decay": 0.2, "sustain": 0, "release": 0.02},
    "f":  {"type": "filter", "mode": "bandpass", "input": "n", "cutoff": {"value": 3000, "jitter": 0.3}, "q": 1.2}
  }, "output": "f", "duration": 1.0})";

constexpr int kRate = 24000;

ear::Clip render(const std::shared_ptr<const SynthGraph>& g, std::vector<std::pair<int, float>> ov, uint32_t seed = 1,
                 bool jitter = false, int rate = kRate) {
    SynthRenderOptions ro;
    ro.sampleRate = rate;
    ro.trigger.seed = seed;
    ro.trigger.jitter = jitter;
    ro.trigger.overrides = std::move(ov);
    ear::Clip c;
    c.samples = renderSynth(g, ro);
    c.sampleRate = rate;
    return c;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

double relErr(double a, double b) { return std::fabs(a - b) / std::fabs(b); }

void printFit(const char* what, const ear::FitResult& r) {
    std::printf("  %s: distance %.5f after %d evaluations (%d generations, %d restarts, lambda %d) in %.2f s, stop %s\n",
                what, r.distance, r.evaluations, r.generations, r.restarts, r.population, r.seconds, r.stop.c_str());
    for (size_t i = 0; i < r.params.size(); ++i) {
        std::printf("    %s = %.5g (searched %.4g .. %.4g %s, start %.4g)\n", r.params[i].name.c_str(), r.values[i],
                    r.params[i].min, r.params[i].max, r.params[i].log ? "log" : "linear", r.params[i].start);
    }
}

int find(const ear::FitResult& r, const std::string& name) {
    for (size_t i = 0; i < r.params.size(); ++i)
        if (r.params[i].name == name) return static_cast<int>(i);
    return -1;
}

} // namespace

TEST(recovers_hidden_parameters_from_a_reference)
{
    auto g = SynthGraph::fromJson(kPluck);
    const int iFreq = g->paramIndex("o.freq"), iDecay = g->paramIndex("ae.decay"), iIdx = g->paramIndex("ie.decay");
    const ear::Clip ref = render(g, {{iFreq, 587.0f}, {iDecay, 0.18f}, {iIdx, 0.07f}});

    ear::FitOptions o;
    o.params = {{"o.freq"}, {"ae.decay"}, {"ie.decay"}};
    o.reference = &ref;
    o.maxEvaluations = 900;
    o.stopAt = 1e-4;
    o.seed = 7;
    const ear::FitResult r = ear::fit(g, o);
    printFit("pluck from a reference", r);
    ASSERT_EQ(r.sampleRate, kRate);
    ASSERT_EQ(r.params.size(), size_t(3));
    ASSERT_TRUE(r.params[0].log && r.params[1].log && r.params[2].log);
    ASSERT_NEAR(r.params[0].min, 110.0, 1e-9);
    ASSERT_NEAR(r.params[0].max, 1760.0, 1e-9);
    ASSERT_TRUE(r.evaluations <= 900);
    ASSERT_LT(r.distance, 0.02);
    ASSERT_LT(relErr(r.values[find(r, "o.freq")], 587.0), 0.01);
    ASSERT_LT(relErr(r.values[find(r, "ae.decay")], 0.18), 0.1);
    ASSERT_LT(relErr(r.values[find(r, "ie.decay")], 0.07), 0.2);
    // The history never gets worse and ends at the result.
    ASSERT_TRUE(!r.history.empty());
    for (size_t i = 1; i < r.history.size(); ++i) ASSERT_TRUE(r.history[i].best <= r.history[i - 1].best);
    ASSERT_EQ(r.history.back().best, r.distance);
    ASSERT_EQ(r.history.back().evaluations, r.evaluations);
    // The clip is the best candidate's render, reproducible from overrides.
    ASSERT_TRUE(sameBits(r.clip.samples, render(g, r.overrides).samples));
    ASSERT_NEAR(r.terms.reference, ear::compare(r.clip, ref).score, 1e-12);
    PASS();
}

TEST(recovers_eight_parameters)
{
    auto g = SynthGraph::fromJson(kPluck);
    const char* names[] = {"o.freq", "o.ratio", "o.feedback", "ie.attack", "ie.decay", "ie.peak", "ae.attack",
                           "ae.decay"};
    const float hidden[] = {392.0f, 3.0f, 0.4f, 0.003f, 0.05f, 4.5f, 0.006f, 0.22f};
    std::vector<std::pair<int, float>> ov;
    ear::FitOptions o;
    for (int i = 0; i < 8; ++i) {
        ov.push_back({g->paramIndex(names[i]), hidden[i]});
        o.params.push_back({names[i]});
    }
    o.params[1].min = 1.0;   // the ratio: 1 .. 8
    o.params[1].max = 8.0;
    o.params[2].max = 1.0;   // feedback 0 .. 1
    const ear::Clip ref = render(g, ov);
    o.reference = &ref;
    o.maxEvaluations = 4000;
    o.stopAt = 0.005;
    o.seed = 1;
    const ear::FitResult r = ear::fit(g, o);
    printFit("pluck, eight parameters", r);
    ASSERT_EQ(r.population, 20);
    ASSERT_LT(r.distance, 0.02);
    ASSERT_LT(relErr(r.values[0], 392.0), 0.01);
    ASSERT_LT(relErr(r.values[1], 3.0), 0.02);
    ASSERT_LT(relErr(r.values[7], 0.22), 0.1);
    PASS();
}

TEST(recovers_a_noise_excited_resonator)
{
    // A noise burst through three modes: the objective is rough (the noise
    // is the same seed's, but the spectrum is not smooth in the ratios).
    auto g = SynthGraph::fromJson(R"({"nodes": {
        "exc": {"type": "noise", "gain": "ee"},
        "ee":  {"type": "env", "attack": 0.0002, "decay": 0.004, "sustain": 0, "release": 0.001},
        "res": {"type": "resonator", "input": "exc", "freq": 520, "modes": [
            {"ratio": 1, "decay": 0.6, "gain": 0.5}, {"ratio": 2.76, "decay": 0.4, "gain": 0.35},
            {"ratio": 5.4, "decay": 0.25, "gain": 0.25}]}
      }, "output": "res", "duration": 1.2})");
    const ear::Clip ref = render(g, {{g->paramIndex("res.freq"), 700.0f},
                                     {g->paramIndex("res.modes.0.decay"), 0.35f},
                                     {g->paramIndex("res.modes.1.decay"), 0.2f},
                                     {g->paramIndex("res.modes.1.ratio"), 3.1f}});
    ear::FitOptions o;
    o.params = {{"res.freq"}, {"res.modes.0.decay"}, {"res.modes.1.decay"}, {"res.modes.1.ratio", 2.0, 4.0}};
    o.reference = &ref;
    o.maxEvaluations = 1500;
    o.stopAt = 0.005;
    o.seed = 2;
    const ear::FitResult r = ear::fit(g, o);
    printFit("resonator from a reference", r);
    ASSERT_LT(r.distance, 0.03);
    ASSERT_LT(relErr(r.values[0], 700.0), 0.01);
    ASSERT_LT(relErr(r.values[3], 3.1), 0.02);
    ASSERT_LT(relErr(r.values[1], 0.35), 0.15);
    PASS();
}

TEST(meets_measurement_and_band_targets)
{
    auto g = SynthGraph::fromJson(kHiss);
    ear::FitOptions o;
    o.params = {{"f.cutoff"}, {"e.decay"}, {"f.q"}};
    o.measures = {{"centroidHz", 1200.0}, {"tailTime", 0.35}};
    o.bands = {{2400.0, 12000.0, -12.0}};
    o.maxEvaluations = 600;
    o.seed = 3;
    const ear::FitResult r = ear::fit(g, o);
    printFit("hiss to measurement targets", r);
    ASSERT_EQ(r.sampleRate, 48000);
    ASSERT_EQ(r.terms.measures.size(), size_t(2));
    ASSERT_EQ(r.terms.bands.size(), size_t(1));
    std::printf("    centroid %.1f Hz, tail %.3f s, band %.2f dB\n", r.terms.measures[0].measured,
                r.terms.measures[1].measured, r.terms.bands[0].measured);
    ASSERT_LT(relErr(r.terms.measures[0].measured, 1200.0), 0.05);
    ASSERT_LT(relErr(r.terms.measures[1].measured, 0.35), 0.1);
    ASSERT_LT(std::fabs(r.terms.bands[0].measured - (-12.0)), 1.0);
    ASSERT_NEAR(r.terms.bands[0].measured, ear::bandLevelDb(r.clip, 2400.0, 12000.0), 1e-9);
    ASSERT_TRUE(std::isnan(r.terms.reference) && std::isnan(r.terms.scorer));
    PASS();
}

TEST(short_click_loudness_and_loops)
{
    // A 25 ms noise click whose level is e.peak: lufsShort moves by the
    // level, and fitting `loudness` (= lufsShort under 400 ms) lands on it.
    auto g = SynthGraph::fromJson(R"({"nodes": {
        "n": {"type": "noise", "gain": "e"},
        "e": {"type": "env", "attack": 0.001, "decay": 0.02, "sustain": 0, "release": 0.005, "peak": 0.2},
        "f": {"type": "filter", "mode": "bandpass", "input": "n", "cutoff": 2500, "q": 1}
      }, "output": "f"})");
    const int iPeak = g->paramIndex("e.peak");
    const ear::Measurement m1 = ear::measure(render(g, {{iPeak, 0.1f}}, 1, false, 48000));
    const ear::Measurement m2 = ear::measure(render(g, {{iPeak, 0.4f}}, 1, false, 48000));
    ASSERT_LT(m2.duration, 0.4);
    ASSERT_NEAR(m2.lufsShort - m1.lufsShort, 12.04, 0.01);
    ASSERT_TRUE(m2.loudness == m2.lufsShort);

    ear::FitOptions o;
    o.params = {{"e.peak", 0.01, 1.0, ear::FitScale::Log}};
    o.measures = {{"loudness", -34.0}};
    o.maxEvaluations = 200;
    o.seed = 3;
    const ear::FitResult r = ear::fit(g, o);
    printFit("click to loudness -34", r);
    ASSERT_LT(r.clip.duration(), 0.4);
    ASSERT_NEAR(ear::measure(r.clip).loudness, -34.0, 0.1);
    ASSERT_NEAR(r.terms.measures[0].measured, ear::measure(r.clip).lufsShort, 1e-9);

    // A loop fit: every candidate renders one period.
    auto hum = SynthGraph::fromJson(R"({"nodes": {
        "s": {"type": "osc", "wave": "saw", "freq": 90},
        "lp": {"type": "filter", "input": "s", "cutoff": 800}}, "output": "lp"})");
    ear::FitOptions lo;
    lo.params = {{"lp.cutoff"}};
    lo.measures = {{"centroidHz", 400.0}};
    lo.loop.length = 0.25;
    lo.maxEvaluations = 60;
    const ear::FitResult lr = ear::fit(hum, lo);
    ASSERT_EQ(lr.clip.samples.size(), size_t(12000));
    SynthRenderOptions ro;
    ro.trigger.overrides = lr.overrides;
    ro.trigger.jitter = false;
    ro.loop = lo.loop;
    ASSERT_TRUE(sameBits(renderSynth(hum, ro), lr.clip.samples));
    PASS();
}

TEST(deterministic_across_thread_counts)
{
    auto g = SynthGraph::fromJson(kHiss);
    ear::FitOptions o;
    o.params = {{"f.cutoff"}, {"e.decay"}};
    o.measures = {{"centroidHz", 2000.0}, {"attackTime", 0.01}};
    o.maxEvaluations = 120;
    o.jitter = true;
    o.seeds = {1, 2, 3};
    o.seed = 11;
    o.threads = 1;
    const ear::FitResult a = ear::fit(g, o);
    o.threads = 7;
    const ear::FitResult b = ear::fit(g, o);
    o.threads = 0;
    const ear::FitResult c = ear::fit(g, o);
    for (const ear::FitResult* x : {&b, &c}) {
        ASSERT_EQ(x->values, a.values);
        ASSERT_EQ(x->distance, a.distance);
        ASSERT_EQ(x->evaluations, a.evaluations);
        ASSERT_EQ(x->history.size(), a.history.size());
        for (size_t i = 0; i < a.history.size(); ++i) {
            ASSERT_EQ(x->history[i].best, a.history[i].best);
            ASSERT_EQ(x->history[i].generationBest, a.history[i].generationBest);
            ASSERT_EQ(x->history[i].sigma, a.history[i].sigma);
        }
        ASSERT_TRUE(sameBits(x->clip.samples, a.clip.samples));
    }
    // Another seed searches differently.
    o.seed = 12;
    const ear::FitResult d = ear::fit(g, o);
    ASSERT_TRUE(d.values != a.values);
    // The clip is seeds[0]'s jittered render.
    ASSERT_EQ(a.sampleRate, 48000);
    ASSERT_TRUE(sameBits(a.clip.samples, render(g, a.overrides, 1, true, 48000).samples));
    ASSERT_FALSE(sameBits(a.clip.samples, render(g, a.overrides, 2, true, 48000).samples));
    PASS();
}

TEST(held_parameters_stay_fixed)
{
    auto g = SynthGraph::fromJson(kPluck);
    const int iIdx = g->paramIndex("ie.decay"), iRatio = g->paramIndex("o.ratio");
    const ear::Clip ref = render(g, {{g->paramIndex("o.freq"), 500.0f}, {iIdx, 0.05f}});
    ear::FitOptions o;
    o.fixed = {{"ie.decay", 0.3}, {"o.ratio", 3.0}};  // every other parameter is searched
    o.reference = &ref;
    o.maxEvaluations = 150;
    const ear::FitResult r = ear::fit(g, o);
    printFit("pluck with ie.decay and o.ratio held", r);
    ASSERT_EQ(find(r, "ie.decay"), -1);
    ASSERT_EQ(find(r, "o.ratio"), -1);
    ASSERT_EQ(r.params.size(), g->params().size() - 2);
    int seenIdx = 0, seenRatio = 0;
    for (const auto& [i, v] : r.overrides) {
        if (i == iIdx) { ASSERT_EQ(v, 0.3f); ++seenIdx; }
        if (i == iRatio) { ASSERT_EQ(v, 3.0f); ++seenRatio; }
    }
    ASSERT_EQ(seenIdx, 1);
    ASSERT_EQ(seenRatio, 1);
    ASSERT_TRUE(sameBits(r.clip.samples, render(g, r.overrides).samples));
    // A parameter outside `params` keeps its declared value: search one, hold none.
    ear::FitOptions one;
    one.params = {{"o.freq", 300.0, 700.0, ear::FitScale::Linear}};
    one.reference = &ref;
    one.maxEvaluations = 64;
    const ear::FitResult s = ear::fit(g, one);
    ASSERT_EQ(s.overrides.size(), size_t(1));
    ASSERT_FALSE(s.params[0].log);
    ASSERT_TRUE(s.values[0] >= 300.0 && s.values[0] <= 700.0);
    PASS();
}

TEST(external_scorer_sees_batches_on_the_calling_thread)
{
    auto g = SynthGraph::fromJson(kPluck);
    ear::FitOptions o;
    o.params = {{"o.freq"}};
    o.maxEvaluations = 200;
    o.population = 10;
    o.seeds = {5, 6};
    o.stopAt = 0.002;
    const std::thread::id caller = std::this_thread::get_id();
    int calls = 0;
    bool wrongThread = false, wrongSize = false;
    ear::FitScorer scorer = [&](const std::vector<const ear::Clip*>& clips, std::vector<double>& out) {
        ++calls;
        wrongThread |= std::this_thread::get_id() != caller;
        wrongSize |= clips.size() != 20 || out.size() != 20;
        for (size_t i = 0; i < clips.size(); ++i) {
            ear::MeasureOptions mo;
            mo.maxPartials = 1;
            const ear::Measurement m = ear::measure(*clips[i], mo);
            out[i] = m.partials.empty() ? 5.0 : std::fabs(std::log2(m.partials[0].freqHz / 330.0));
        }
        return true;
    };
    const ear::FitResult r = ear::fit(g, o, scorer);
    printFit("pluck to a scorer's 330 Hz", r);
    ASSERT_FALSE(wrongThread);
    ASSERT_FALSE(wrongSize);
    ASSERT_EQ(calls, r.generations);
    ASSERT_LT(relErr(r.values[0], 330.0), 0.01);
    ASSERT_NEAR(r.terms.scorer, r.distance, 1e-12);
    // A scorer that returns false stops the fit.
    int n = 0;
    const ear::FitResult stopped = ear::fit(g, o, [&](const std::vector<const ear::Clip*>&, std::vector<double>& out) {
        std::fill(out.begin(), out.end(), 1.0);
        return ++n < 3;
    });
    ASSERT_EQ(stopped.stop, std::string("scorer"));
    ASSERT_EQ(stopped.generations, 2);
    PASS();
}

TEST(progress_cancels_and_time_budget_stops)
{
    auto g = SynthGraph::fromJson(kPluck);
    const ear::Clip ref = render(g, {{g->paramIndex("o.freq"), 500.0f}});
    ear::FitOptions o;
    o.params = {{"o.freq"}, {"ae.decay"}};
    o.reference = &ref;
    o.maxEvaluations = 100000;
    int seen = 0;
    const ear::FitResult c = ear::fit(g, o, {}, [&](const ear::FitProgress& p) {
        ++seen;
        return !(p.generation == 3 && p.bestValues && p.bestValues->size() == 2);
    });
    ASSERT_EQ(c.stop, std::string("cancelled"));
    ASSERT_EQ(c.generations, 3);
    ASSERT_EQ(seen, 3);

    o.maxSeconds = 0.3;
    const auto t0 = std::chrono::steady_clock::now();
    const ear::FitResult t = ear::fit(g, o);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  time budget 0.3 s: %d evaluations in %.2f s, stop %s\n", t.evaluations, took, t.stop.c_str());
    ASSERT_EQ(t.stop, std::string("time"));
    ASSERT_LT(took, 3.0);
    PASS();
}

TEST(bad_options_name_the_field)
{
    auto g = SynthGraph::fromJson(kPluck);
    const ear::Clip ref = render(g, {});
    auto message = [&](ear::FitOptions o, bool range) -> std::string {
        o.maxEvaluations = 8;
        try {
            ear::fit(g, o);
        } catch (const std::out_of_range& e) {
            return range ? e.what() : std::string("wrong kind: ") + e.what();
        } catch (const std::invalid_argument& e) {
            return range ? std::string("wrong kind: ") + e.what() : e.what();
        }
        return "no throw";
    };
    ear::FitOptions o;
    ASSERT_TRUE(message(o, false).find("a target is required") == 0);
    o.reference = &ref;
    o.params = {{"o.frq"}};
    ASSERT_TRUE(message(o, false).find("params.o.frq: unknown parameter") == 0);
    o.params = {{"o.freq", 500.0, 400.0}};
    ASSERT_TRUE(message(o, true).find("params.o.freq: the range") == 0);
    o.params = {{"o.freq", -5.0}};
    ASSERT_TRUE(message(o, true).find("params.o.freq.min") == 0);
    o.params = {{"o.freq"}};
    o.fixed = {{"o.freq", 300.0}};
    ASSERT_TRUE(message(o, false).find("params.o.freq: both searched and fixed") == 0);
    o.fixed = {};
    o.measures = {{"brightness", 1.0}};
    ASSERT_TRUE(message(o, false).find("measures.brightness: unknown measurement") == 0);
    o.measures = {};
    o.bands = {{500.0, 100.0, -3.0}};
    ASSERT_TRUE(message(o, true).find("bands.0") == 0);
    PASS();
}

int main() { return runAllTests(); }
