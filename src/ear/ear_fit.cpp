// ear::fit (include/broaudio/ear/fit.h): the parameters, the parallel
// render-and-score of a generation, and the CMA-ES loop with IPOP restarts.
//
// Determinism: the search's random numbers are drawn on the calling thread
// only (ear_cmaes.h); worker threads render and score candidates whose
// results land at fixed indices; ties rank by index. So nothing depends on
// how many threads there are or which finishes first.

#include "broaudio/ear/fit.h"
#include "broaudio/synth/synth_graph.h"
#include "ear_cmaes.h"
#include "ear_fit_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace broaudio::ear {

using namespace detail;

namespace {

std::string fmt(double v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

[[noreturn]] void bad(const std::string& msg) { throw std::invalid_argument(msg); }
[[noreturn]] void outOfRange(const std::string& msg) { throw std::out_of_range(msg); }

struct Plan {
    std::vector<FitParamInfo> params;
    std::vector<std::pair<int, float>> fixed;
    std::vector<double> start;  // unit cube
};

double toUnit(const FitParamInfo& p, double v) {
    if (p.log) return (std::log(v) - std::log(p.min)) / (std::log(p.max) - std::log(p.min));
    return (v - p.min) / (p.max - p.min);
}

float fromUnit(const FitParamInfo& p, double x, const SynthParamInfo& g) {
    x = std::clamp(x, 0.0, 1.0);
    double v = p.log ? std::exp(std::log(p.min) + x * (std::log(p.max) - std::log(p.min)))
                     : p.min + x * (p.max - p.min);
    v = std::clamp(v, p.min, p.max);
    float f = static_cast<float>(v);
    return std::clamp(f, g.lo, g.hi);
}

void defaultRange(const SynthParamInfo& g, double base, FitParamSpec& s, bool& log) {
    const double lo = g.lo, hi = g.hi;
    if (s.scale == FitScale::Auto) {
        log = base > 0.0 && lo >= 0.0 && hi >= 10.0;
    } else {
        log = s.scale == FitScale::Log;
    }
    double dmin, dmax;
    if (log && base > 0.0) {
        dmin = base / 4.0;
        dmax = base * 4.0;
    } else if (log) {
        dmin = std::max(lo, 1e-3);
        dmax = std::min(hi, 1.0);
    } else if (hi - lo <= 10.0) {
        dmin = lo;
        dmax = hi;
    } else if (base != 0.0) {
        dmin = base - std::fabs(base);
        dmax = base + std::fabs(base);
    } else if (lo >= 0.0) {
        dmin = 0.0;
        dmax = 1.0;
    } else {
        dmin = -1.0;
        dmax = 1.0;
    }
    if (std::isnan(s.min)) s.min = std::max(dmin, lo);
    if (std::isnan(s.max)) s.max = std::min(dmax, hi);
}

Plan plan(const SynthGraph& g, const FitOptions& o) {
    Plan p;
    const auto& infos = g.params();
    std::vector<int> fixedAt(infos.size(), 0);
    std::vector<double> fixedValue(infos.size(), 0.0);
    for (const auto& [name, value] : o.fixed) {
        const std::string path = "fixed." + name;
        const int idx = g.paramIndex(name);
        if (idx < 0) bad(path + ": unknown parameter");
        if (fixedAt[idx]) bad(path + ": given twice");
        const SynthParamInfo& gi = infos[idx];
        if (!std::isfinite(value) || value < gi.lo || value > gi.hi) {
            outOfRange(path + ": " + fmt(value) + " is outside the parameter's range [" + fmt(gi.lo) + ", " +
                       fmt(gi.hi) + "]");
        }
        fixedAt[idx] = 1;
        fixedValue[idx] = value;
        p.fixed.push_back({idx, static_cast<float>(value)});
    }

    std::vector<FitParamSpec> specs = o.params;
    if (specs.empty()) {
        for (size_t i = 0; i < infos.size(); ++i) {
            if (!fixedAt[i]) specs.push_back(FitParamSpec{infos[i].name});
        }
    }
    if (specs.empty()) bad("params: nothing to search");
    if (specs.size() > 64) bad("params: at most 64 parameters (" + std::to_string(specs.size()) + " given)");
    std::vector<int> seen(infos.size(), 0);
    for (FitParamSpec s : specs) {
        const std::string path = "params." + s.name;
        const int idx = g.paramIndex(s.name);
        if (idx < 0) bad(path + ": unknown parameter");
        if (fixedAt[idx]) bad(path + ": both searched and fixed");
        if (seen[idx]) bad(path + ": given twice");
        seen[idx] = 1;
        const SynthParamInfo& gi = infos[idx];
        for (const auto& [what, v] : {std::pair<const char*, double>{"min", s.min}, {"max", s.max}}) {
            if (!std::isnan(v) && (!std::isfinite(v) || v < gi.lo || v > gi.hi)) {
                outOfRange(path + "." + what + ": " + fmt(v) + " is outside the parameter's range [" + fmt(gi.lo) +
                           ", " + fmt(gi.hi) + "]");
            }
        }
        const double base = gi.value;
        bool log = false;
        defaultRange(gi, base, s, log);
        if (!(s.min < s.max)) outOfRange(path + ": the range [" + fmt(s.min) + ", " + fmt(s.max) + "] is empty");
        if (log && !(s.min > 0.0)) outOfRange(path + ": a log range needs min > 0 (min is " + fmt(s.min) + ")");
        FitParamInfo info;
        info.name = s.name;
        info.index = idx;
        info.min = s.min;
        info.max = s.max;
        info.log = log;
        if (!std::isnan(s.start)) {
            if (!(s.start >= s.min && s.start <= s.max)) {
                outOfRange(path + ".start: " + fmt(s.start) + " is outside [" + fmt(s.min) + ", " + fmt(s.max) + "]");
            }
            info.start = s.start;
        } else {
            info.start = std::clamp(base, s.min, s.max);
        }
        p.start.push_back(std::clamp(toUnit(info, info.start), 0.0, 1.0));
        p.params.push_back(info);
    }
    return p;
}

void validate(const FitOptions& o, bool hasScorer) {
    if (!o.reference && o.measures.empty() && o.bands.empty() && !hasScorer) {
        bad("a target is required: a reference, measures, bands or a scorer");
    }
    if (o.reference && (o.reference->sampleRate <= 0 || o.reference->samples.empty())) {
        bad("reference: an empty clip");
    }
    validateMeasureTargets(o.measures);
    for (size_t i = 0; i < o.bands.size(); ++i) {
        const FitBandTarget& b = o.bands[i];
        const std::string path = "bands." + std::to_string(i);
        if (!(b.minHz >= 0.0 && b.maxHz > b.minHz && std::isfinite(b.maxHz))) {
            outOfRange(path + ": needs 0 <= minHz < maxHz");
        }
        if (!std::isfinite(b.db)) bad(path + ".db: must be a finite number");
        if (!(b.weight >= 0.0 && std::isfinite(b.weight))) bad(path + ".weight: must be a finite number >= 0");
    }
    auto weight = [](const char* name, double w) {
        if (!(w >= 0.0 && std::isfinite(w))) bad(std::string(name) + ": must be a finite number >= 0");
    };
    weight("weights.reference", o.referenceWeight);
    weight("weights.scorer", o.scorerWeight);
    if (o.maxEvaluations < 1) outOfRange("maxEvaluations: must be at least 1");
    if (!(o.maxSeconds >= 0.0)) outOfRange("maxSeconds: must be >= 0");
    if (!(o.sigma > 0.0 && o.sigma <= 1.0)) outOfRange("sigma: must be in (0, 1]");
    if (o.population < 0 || o.population > 4096) outOfRange("population: 0 (automatic) .. 4096");
    if (o.seeds.empty() || o.seeds.size() > 64) outOfRange("seeds: 1 .. 64 render seeds");
}

// Runs fn(0..count) on `threads` threads (the caller's among them).
template <class Fn>
void parallelFor(int count, int threads, Fn&& fn) {
    std::atomic<int> next{0};
    auto work = [&] {
        for (int i = next.fetch_add(1); i < count; i = next.fetch_add(1)) fn(i);
    };
    const int extra = std::min(threads, count) - 1;
    std::vector<std::thread> pool;
    pool.reserve(std::max(0, extra));
    for (int t = 0; t < extra; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}

} // namespace

std::vector<FitParamInfo> planFit(const std::shared_ptr<const SynthGraph>& graph, const FitOptions& o,
                                  bool hasScorer) {
    if (!graph) bad("graph: required");
    validate(o, hasScorer);
    return plan(*graph, o).params;
}

FitResult fit(const std::shared_ptr<const SynthGraph>& graph, const FitOptions& o, const FitScorer& scorer,
              const FitProgressFn& progress) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };

    if (!graph) bad("graph: required");
    validate(o, static_cast<bool>(scorer));
    const Plan pl = plan(*graph, o);
    const auto& infos = graph->params();
    const int n = static_cast<int>(pl.params.size());
    const int S = static_cast<int>(o.seeds.size());

    FitResult r;
    r.params = pl.params;
    r.sampleRate = o.sampleRate > 0 ? o.sampleRate : (o.reference ? o.reference->sampleRate : 48000);
    r.sampleRate = std::clamp(r.sampleRate, 8000, 384000);
    const double maxDuration = o.maxDuration > 0.0 ? o.maxDuration
                               : o.reference       ? std::min(600.0, 2.0 * o.reference->duration() + 0.25)
                                                   : 10.0;
    r.maxDuration = maxDuration;
    int threads = o.threads > 0 ? o.threads : static_cast<int>(std::thread::hardware_concurrency());
    threads = std::clamp(threads, 1, 32);
    if (o.compiled) graph->precompile();

    FitRng rng(o.seed);
    int lambda = o.population;
    auto cma = std::make_unique<Cmaes>(pl.start, o.sigma, lambda);
    if (cma->lambda() > o.maxEvaluations) {
        cma = std::make_unique<Cmaes>(pl.start, o.sigma, std::max(2, o.maxEvaluations));
    }
    r.population = cma->lambda();

    auto overridesFor = [&](const std::vector<double>& x, std::vector<double>* values) {
        std::vector<std::pair<int, float>> ov = pl.fixed;
        for (int i = 0; i < n; ++i) {
            const float v = fromUnit(pl.params[i], x[i], infos[pl.params[i].index]);
            ov.push_back({pl.params[i].index, v});
            if (values) values->push_back(v);
        }
        return ov;
    };

    std::vector<double> bestX;
    double bestScorer = NAN;
    r.stop = "evaluations";
    for (;;) {
        const int lam = cma->lambda();
        if (r.evaluations + lam > o.maxEvaluations) {
            r.stop = "evaluations";
            break;
        }
        if (r.generations > 0 && o.maxSeconds > 0.0 && elapsed() >= o.maxSeconds) {
            r.stop = "time";
            break;
        }
        const std::vector<std::vector<double>> xs = cma->ask(rng);
        const int tasks = lam * S;
        std::vector<Clip> clips(tasks);
        std::vector<double> internal(tasks, 0.0);
        parallelFor(tasks, threads, [&](int t) {
            const int k = t / S, s = t % S;
            SynthRenderOptions ro;
            ro.sampleRate = r.sampleRate;
            ro.maxSeconds = maxDuration;
            ro.compiled = o.compiled;
            ro.loop = o.loop;
            ro.trigger.seed = o.seeds[s];
            ro.trigger.jitter = o.jitter;
            ro.trigger.overrides = overridesFor(xs[k], nullptr);
            Clip& c = clips[t];
            c.samples = renderSynth(graph, ro);
            c.sampleRate = r.sampleRate;
            c.channels = 1;
            internal[t] = fitDistance(c, o, nullptr);
        });
        std::vector<double> external(tasks, 0.0);
        if (scorer) {
            std::vector<const Clip*> ptrs(tasks);
            for (int t = 0; t < tasks; ++t) ptrs[t] = &clips[t];
            external.assign(tasks, 0.0);
            if (!scorer(ptrs, external)) {
                r.stop = "scorer";
                break;
            }
            external.resize(tasks, NAN);
        }
        std::vector<double> fitness(lam, 0.0);
        double genBest = INFINITY;
        for (int k = 0; k < lam; ++k) {
            double sum = 0.0, ext = 0.0;
            for (int s = 0; s < S; ++s) {
                const int t = k * S + s;
                sum += internal[t];
                if (scorer) {
                    const double e = std::isfinite(external[t]) ? external[t] : kFitMissing;
                    sum += o.scorerWeight * e;
                    ext += e;
                }
            }
            fitness[k] = std::isfinite(sum) ? sum / S : INFINITY;
            genBest = std::min(genBest, fitness[k]);
            if (fitness[k] < r.distance || bestX.empty()) {
                r.distance = fitness[k];
                bestX = xs[k];
                bestScorer = scorer ? ext / S : NAN;
                r.clip = std::move(clips[k * S]);
            }
        }
        cma->tell(xs, fitness);
        r.evaluations += lam;
        ++r.generations;
        FitHistoryEntry h;
        h.generation = r.generations;
        h.evaluations = r.evaluations;
        h.best = r.distance;
        h.generationBest = genBest;
        h.sigma = cma->sigma();
        h.seconds = elapsed();
        r.history.push_back(h);
        if (progress) {
            std::vector<double> bestValues;
            overridesFor(bestX, &bestValues);
            FitProgress pg;
            pg.generation = r.generations;
            pg.evaluations = r.evaluations;
            pg.restarts = r.restarts;
            pg.best = r.distance;
            pg.generationBest = genBest;
            pg.sigma = cma->sigma();
            pg.seconds = h.seconds;
            pg.bestValues = &bestValues;
            pg.params = &pl.params;
            if (!progress(pg)) {
                r.stop = "cancelled";
                break;
            }
        }
        if (r.distance <= o.stopAt) {
            r.stop = "stopAt";
            break;
        }
        if (cma->converged()) {
            const int next = std::min(4096, cma->lambda() * 2);
            if (!o.restarts || r.evaluations + next > o.maxEvaluations) {
                r.stop = "converged";
                break;
            }
            std::vector<double> x(n);
            for (double& v : x) v = rng.uniform();
            cma = std::make_unique<Cmaes>(x, o.sigma, next);
            ++r.restarts;
        }
    }

    if (!bestX.empty()) {
        r.overrides = overridesFor(bestX, &r.values);
        fitDistance(r.clip, o, &r.terms);
        r.terms.scorer = bestScorer;
    } else {
        r.overrides = pl.fixed;
        for (const FitParamInfo& p : pl.params) {
            const float v = static_cast<float>(p.start);
            r.values.push_back(v);
            r.overrides.push_back({p.index, v});
        }
    }
    r.seconds = elapsed();
    return r;
}

} // namespace broaudio::ear
