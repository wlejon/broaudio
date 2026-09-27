#pragma once

// ear::fit: search a synthesis graph's parameters until its render sounds
// like a target.
//
// Given a SynthGraph (synth/synth_graph.h), the parameters to search (with a
// range and a scale each, defaulted from the graph) and a target, fit()
// minimises one distance over the parameters with CMA-ES and returns the
// best parameters, the score history and the best render. The distance is a
// weighted sum of
//
//   - the compare() score against a reference clip,
//   - measurement targets: a value per measure() field (centroidHz, t60,
//     attackTime, lufs, tonality, ...), each turned into a unitless distance
//     where 1 is "clearly different" (an octave, a factor of 2 in time,
//     10 dB, a quarter of a 0..1 fraction),
//   - band levels: a band's share of the clip's power, dB,
//   - an external scorer: any function of a batch of clips, called on the
//     thread that called fit() (the JS binding's hook for a JS function or a
//     CLAP model, which broaudio cannot link).
//
// SEARCH. CMA-ES (Hansen's (mu/mu_w, lambda) with rank-one and rank-mu
// updates and cumulative step-size adaptation) in a unit cube: each
// parameter is mapped from its range, linearly or logarithmically, onto
// [0, 1], samples are reflected back into the cube, and the run restarts
// with a doubled population from a random point (IPOP) when it converges
// before the budget is spent. A generation's candidates render and score in
// parallel on `threads` native threads; everything the search decides comes
// from one seeded generator on the calling thread and from each candidate's
// own deterministic render, so a given seed gives the same parameters, the
// same history and the same clip, bit for bit, for any thread count. A time
// budget stops the run at a generation boundary, so a timed run is a prefix
// of the untimed one.
//
// JITTER. Renders use jitter off by default: the objective is then a
// deterministic function of the parameters. With `jitter` on, each candidate
// is rendered once per seed in `seeds` and its distance is the mean, the
// same seeds for every candidate (common random numbers), so the objective
// stays deterministic and the fit favours parameters that sound right across
// the graph's variation. Noise generators draw from the seed either way.
//
// Not for the audio thread: it allocates, spawns threads and blocks.

#include "broaudio/ear/ear.h"
#include "broaudio/synth/synth_graph.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace broaudio {
class SynthGraph;
}

namespace broaudio::ear {

enum class FitScale : uint8_t { Auto, Linear, Log };

// One parameter to search. NaN min / max / start take the defaults:
//   scale Auto: Log when the declared value is > 0 and the graph's range is
//     non-negative and reaches at least 10 (frequencies, times, ratios, q,
//     drive); Linear otherwise.
//   Log range: declared value / 4 .. declared value * 4 (two octaves either
//     way), inside the graph's range.
//   Linear range: the graph's whole range when it spans at most 10 (pulse
//     width, damping, feedback); else declared value -/+ |value| (so the
//     sign is kept: a gain of 0.5 searches 0 .. 1), or -1 .. 1 around 0,
//     inside the graph's range.
//   start: the declared value (or the `fixed` value), clamped into the range.
struct FitParamSpec {
    std::string name;
    double min = NAN;
    double max = NAN;
    FitScale scale = FitScale::Auto;
    double start = NAN;
};

// The parameter as fit() searched it, defaults resolved.
struct FitParamInfo {
    std::string name;
    int index = -1;  // in SynthGraph::params()
    double min = 0, max = 0;
    bool log = false;
    double start = 0;
};

// A measure() field and the value wanted. `name` is one of
//   Hz (distance |log2(measured / target)|, octaves):
//     centroidHz, f0Hz (ringing.f0Hz), partialHz (the strongest partial)
//   seconds (|log2((measured + 2 ms) / (target + 2 ms))|, factors of 2):
//     attackTime, onsetTime, envelopePeakTime, peakTime, tailTime, t60,
//     weightedRingTime, strongestRingTime
//   dB/s (|log2((|measured| + 1) / (|target| + 1))|): decayRate
//   dB (|measured - target| / 10): peakDb, envelopePeakDb, rmsDb, lufs,
//     lufsShort, loudness. For a cue that may render shorter than 400 ms,
//     target `loudness` (or `lufsShort`): BS.1770's `lufs` of a sub-block
//     clip is its energy over its own length, so a fit to it trades level
//     against decay and trailing silence (ear.h, Measurement::lufsShort).
//   0..1 fractions (|measured - target| / 0.25): flatness, tonality,
//     inharmonicity, ringScore, sparsity
// `scale` (NaN = the default above: 1, 1, 1, 10, 0.25) divides the raw
// difference instead. A measured value that does not exist (a t60 of a
// sound that does not decay, the LUFS of silence) costs kFitMissing.
struct FitMeasureTarget {
    std::string name;
    double value = 0;
    double weight = 1;
    double scale = NAN;
};

// A band's share of the clip's power: 10 log10(band power / total power) of
// the long-term power spectrum (Hann STFT, ~43 ms), dB. Distance
// |measured - db| / 10.
struct FitBandTarget {
    double minHz = 0;
    double maxHz = 0;
    double db = 0;
    double weight = 1;
};

inline constexpr double kFitMissing = 2.0;

// The external scorer: one distance per clip (lower is closer), into
// `distances` (resized to clips.size() by the caller). Called once per
// generation, with every candidate's clips (population x seeds, candidate
// major), on the thread that called fit(). Return false to stop the fit
// (FitResult::stop = "scorer"). Its distance joins the sum times
// FitOptions::scorerWeight; the seeds' distances average per candidate.
using FitScorer = std::function<bool(const std::vector<const Clip*>& clips, std::vector<double>& distances)>;

struct FitProgress {
    int generation = 0;         // generations completed
    int evaluations = 0;        // candidates evaluated
    int restarts = 0;
    double best = INFINITY;     // best distance so far
    double generationBest = INFINITY;
    double sigma = 0;           // step size, unit-cube units
    double seconds = 0;         // since fit() started
    const std::vector<double>* bestValues = nullptr;  // per searched parameter
    const std::vector<FitParamInfo>* params = nullptr;
};

// Return false to cancel (FitResult::stop = "cancelled"). Called on the
// thread that called fit(), after every generation.
using FitProgressFn = std::function<bool(const FitProgress&)>;

struct FitOptions {
    std::vector<FitParamSpec> params;  // empty: every parameter not in `fixed`
    std::vector<std::pair<std::string, double>> fixed;  // held at this value

    // Targets. At least one of: a reference, a measure, a band, a scorer.
    const Clip* reference = nullptr;
    CompareOptions compare;
    double referenceWeight = 1;
    std::vector<FitMeasureTarget> measures;
    std::vector<FitBandTarget> bands;
    double scorerWeight = 1;

    // Budget: the fit stops when the next generation would exceed
    // maxEvaluations, after maxSeconds (0 = none), or when the best distance
    // reaches stopAt.
    int maxEvaluations = 1000;
    double maxSeconds = 0;
    double stopAt = -INFINITY;

    // Search.
    int population = 0;   // lambda; 0 = 2 (4 + floor(3 ln n)), at least 10 (restarts double it)
    double sigma = 0.25;  // initial step, unit-cube units
    uint64_t seed = 1;
    int threads = 0;      // 0 = hardware concurrency (at most 32)
    bool restarts = true;

    // Rendering.
    bool jitter = false;
    std::vector<uint32_t> seeds{1};  // render seeds, averaged
    int sampleRate = 0;              // 0 = the reference's, else 48000
    double maxDuration = 0;          // 0 = 2 x the reference + 0.25 s, else 10 s
    bool compiled = true;
    // Enabled: every candidate renders one period of this loop
    // (SynthRenderOptions::loop) instead of a one-shot; maxDuration is
    // unused.
    SynthLoopOptions loop;
};

struct FitMeasureResult {
    std::string name;
    double target = 0;
    double measured = NAN;
    double distance = 0;    // unweighted
};

struct FitBandResult {
    double minHz = 0, maxHz = 0, target = 0;
    double measured = NAN;
    double distance = 0;
};

// The best candidate's distance, term by term, unweighted: the reference,
// measure and band terms of its render at seeds[0] (FitResult::clip), the
// scorer's distance averaged over the seeds. NaN (or empty) for a term the
// fit did not use.
struct FitTerms {
    double reference = NAN;
    Comparison comparison;  // against the reference
    std::vector<FitMeasureResult> measures;
    std::vector<FitBandResult> bands;
    double scorer = NAN;
};

struct FitHistoryEntry {
    int generation = 0;
    int evaluations = 0;
    double best = INFINITY;
    double generationBest = INFINITY;
    double sigma = 0;
    double seconds = 0;
};

struct FitResult {
    std::vector<FitParamInfo> params;  // the searched parameters
    std::vector<double> values;        // best value per searched parameter (as rendered: float)
    // Every override that reproduces `clip`: the searched values and the
    // fixed ones. SynthRenderOptions{trigger: {seeds[0], jitter, overrides}}
    // renders the same samples.
    std::vector<std::pair<int, float>> overrides;
    double distance = INFINITY;
    FitTerms terms;
    int evaluations = 0;
    int generations = 0;
    int restarts = 0;
    int population = 0;  // lambda of the first run
    double seconds = 0;
    // "evaluations", "time", "stopAt", "converged" (no budget left for a
    // restart, or restarts off), "cancelled", "scorer"
    std::string stop;
    std::vector<FitHistoryEntry> history;
    Clip clip;  // the best candidate's render at seeds[0]
    int sampleRate = 0;
    double maxDuration = 0;  // the renders' length cap, resolved
};

// Throws std::invalid_argument for bad options (an unknown parameter or
// measurement, no target) and std::out_of_range (a subclass of logic_error,
// not of invalid_argument) for a number outside its range (an empty or
// inverted range, a log range reaching 0, a fixed value the graph rejects);
// the message starts with the offending field ("params.body.cutoff: ...").
FitResult fit(const std::shared_ptr<const SynthGraph>& graph, const FitOptions& options,
              const FitScorer& scorer = {}, const FitProgressFn& progress = {});

// What fit() would search, defaults resolved, validating the options exactly
// as fit() does (and throwing the same errors) without rendering anything.
std::vector<FitParamInfo> planFit(const std::shared_ptr<const SynthGraph>& graph, const FitOptions& options,
                                  bool hasScorer);

// The distance of one clip to the non-external targets of `options`
// (reference, measures, bands; weighted and summed), what fit() adds the
// scorer to. `terms` (optional) receives the breakdown.
double fitDistance(const Clip& clip, const FitOptions& options, FitTerms* terms = nullptr);

// The long-term band level fitBandTarget describes.
double bandLevelDb(const Clip& clip, double minHz, double maxHz);

} // namespace broaudio::ear
