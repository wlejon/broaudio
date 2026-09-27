// ear::fit's distance (include/broaudio/ear/fit.h): the reference
// comparison, measurement targets and band levels of one clip, weighted and
// summed. Pure computation, safe on any thread.

#include "broaudio/ear/fit.h"
#include "ear_fit_internal.h"
#include "ear_internal.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace broaudio::ear {

using namespace detail;

namespace {

enum class Kind : uint8_t { Hz, Seconds, Rate, Db, Fraction };

struct MeasureField {
    const char* name;
    Kind kind;
    double (*get)(const Measurement&);
};

double strongestPartialHz(const Measurement& m) {
    return m.partials.empty() ? NAN : m.partials.front().freqHz;
}

const MeasureField kFields[] = {
    {"centroidHz", Kind::Hz, [](const Measurement& m) { return m.centroidHz; }},
    {"f0Hz", Kind::Hz, [](const Measurement& m) { return m.ringing.f0Hz; }},
    {"partialHz", Kind::Hz, strongestPartialHz},
    {"attackTime", Kind::Seconds, [](const Measurement& m) { return m.attackTime; }},
    {"onsetTime", Kind::Seconds, [](const Measurement& m) { return m.onsetTime; }},
    {"envelopePeakTime", Kind::Seconds, [](const Measurement& m) { return m.envelopePeakTime; }},
    {"peakTime", Kind::Seconds, [](const Measurement& m) { return m.peakTime; }},
    {"tailTime", Kind::Seconds, [](const Measurement& m) { return m.tailTime; }},
    {"t60", Kind::Seconds, [](const Measurement& m) { return m.t60; }},
    {"weightedRingTime", Kind::Seconds, [](const Measurement& m) { return m.ringing.weightedRingTime; }},
    {"strongestRingTime", Kind::Seconds, [](const Measurement& m) { return m.ringing.strongestRingTime; }},
    {"decayRate", Kind::Rate, [](const Measurement& m) { return m.decayRate; }},
    {"peakDb", Kind::Db, [](const Measurement& m) { return m.peakDb; }},
    {"envelopePeakDb", Kind::Db, [](const Measurement& m) { return m.envelopePeakDb; }},
    {"rmsDb", Kind::Db, [](const Measurement& m) { return m.rmsDb; }},
    {"lufs", Kind::Db, [](const Measurement& m) { return m.lufs; }},
    {"flatness", Kind::Fraction, [](const Measurement& m) { return m.flatness; }},
    {"tonality", Kind::Fraction, [](const Measurement& m) { return m.tonality; }},
    {"inharmonicity", Kind::Fraction, [](const Measurement& m) { return m.ringing.inharmonicity; }},
    {"ringScore", Kind::Fraction, [](const Measurement& m) { return m.ringing.ringScore; }},
    {"sparsity", Kind::Fraction, [](const Measurement& m) { return m.ringing.sparsity; }},
};

const MeasureField* findField(const std::string& name) {
    for (const MeasureField& f : kFields) {
        if (name == f.name) return &f;
    }
    return nullptr;
}

double defaultScale(Kind k) {
    switch (k) {
    case Kind::Db: return 10.0;
    case Kind::Fraction: return 0.25;
    default: return 1.0;
    }
}

// The unitless distance of `measured` from `target`, before the scale.
double rawDistance(Kind k, double measured, double target) {
    switch (k) {
    case Kind::Hz:
        if (!(measured > 0.0) || !(target > 0.0)) return NAN;
        return std::fabs(std::log2(measured / target));
    case Kind::Seconds:
        return std::fabs(std::log2((std::max(0.0, measured) + 0.002) / (std::max(0.0, target) + 0.002)));
    case Kind::Rate:
        return std::fabs(std::log2((std::fabs(measured) + 1.0) / (std::fabs(target) + 1.0)));
    case Kind::Db:
    case Kind::Fraction:
        return std::fabs(measured - target);
    }
    return NAN;
}

// Band power / total power of the long-term spectrum, dB, for each band.
std::vector<double> bandLevels(const Clip& clip, const std::vector<std::pair<double, double>>& bands) {
    std::vector<double> out(bands.size(), kFloorDb);
    if (clip.sampleRate <= 0 || clip.samples.empty()) return out;
    const int nfft = defaultFftSize(clip.sampleRate);
    const Stft s = computeStft(clip, nfft, nfft / 4);
    std::vector<double> ltas(s.bins, 0.0);
    for (int f = 0; f < s.frames; ++f) {
        const float* p = s.frame(f);
        for (int b = 0; b < s.bins; ++b) ltas[b] += p[b];
    }
    double total = 0.0;
    for (double v : ltas) total += v;
    if (!(total > 0.0)) return out;
    for (size_t i = 0; i < bands.size(); ++i) {
        double sum = 0.0;
        for (int b = 0; b < s.bins; ++b) {
            const double hz = b * s.binHz;
            if (hz >= bands[i].first && hz < bands[i].second) sum += ltas[b];
        }
        out[i] = sum > 0.0 ? std::max(kFloorDb, 10.0 * std::log10(sum / total)) : kFloorDb;
    }
    return out;
}

} // namespace

namespace detail {

void validateMeasureTargets(const std::vector<FitMeasureTarget>& measures) {
    for (const FitMeasureTarget& t : measures) {
        const std::string path = "measures." + t.name;
        const MeasureField* f = findField(t.name);
        if (!f) {
            std::string list;
            for (const MeasureField& k : kFields) list += (list.empty() ? "" : ", ") + std::string(k.name);
            throw std::invalid_argument(path + ": unknown measurement (expected one of " + list + ")");
        }
        if (!std::isfinite(t.value)) throw std::invalid_argument(path + ": the target must be a finite number");
        if (f->kind == Kind::Hz && !(t.value > 0.0)) throw std::invalid_argument(path + ": the target must be > 0 Hz");
        if (!(t.weight >= 0.0) || !std::isfinite(t.weight)) {
            throw std::invalid_argument(path + ".weight: must be a finite number >= 0");
        }
        if (!std::isnan(t.scale) && !(t.scale > 0.0 && std::isfinite(t.scale))) {
            throw std::invalid_argument(path + ".scale: must be a finite number > 0");
        }
    }
}

} // namespace detail

double bandLevelDb(const Clip& clip, double minHz, double maxHz) {
    return bandLevels(clip, {{minHz, maxHz}})[0];
}

double fitDistance(const Clip& clip, const FitOptions& o, FitTerms* terms) {
    double total = 0.0;
    if (o.reference) {
        const Comparison c = compare(clip, *o.reference, o.compare);
        const double d = std::isfinite(c.score) ? c.score : kFitMissing;
        total += o.referenceWeight * d;
        if (terms) {
            terms->reference = d;
            terms->comparison = c;
        }
    }
    if (!o.measures.empty()) {
        MeasureOptions mo;
        mo.slices = 1;
        mo.maxPartials = 1;
        const Measurement m = measure(clip, mo);
        if (terms) terms->measures.clear();
        for (const FitMeasureTarget& t : o.measures) {
            const MeasureField* f = findField(t.name);
            if (!f) continue;
            const double v = f->get(m);
            double d = std::isfinite(v) ? rawDistance(f->kind, v, t.value) : NAN;
            d = std::isfinite(d) ? d / (std::isnan(t.scale) ? defaultScale(f->kind) : t.scale) : kFitMissing;
            total += t.weight * d;
            if (terms) terms->measures.push_back({t.name, t.value, v, d});
        }
    }
    if (!o.bands.empty()) {
        std::vector<std::pair<double, double>> edges;
        for (const FitBandTarget& b : o.bands) edges.push_back({b.minHz, b.maxHz});
        const std::vector<double> lv = bandLevels(clip, edges);
        if (terms) terms->bands.clear();
        for (size_t i = 0; i < o.bands.size(); ++i) {
            const FitBandTarget& b = o.bands[i];
            const double d = std::fabs(lv[i] - b.db) / 10.0;
            total += b.weight * d;
            if (terms) terms->bands.push_back({b.minHz, b.maxHz, b.db, lv[i], d});
        }
    }
    return std::isfinite(total) ? total : INFINITY;
}

} // namespace broaudio::ear
