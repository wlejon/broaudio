// measure(): one clip's report. See ear.h for every field's definition.

#include "ear_internal.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace broaudio::ear {

using namespace detail;

namespace {

// Timing from the envelope, plus the Schroeder decay over the tail.
void measureTiming(const Clip& clip, const Envelope& env, const MeasureOptions& opts,
                   Measurement& m) {
    if (env.db.empty()) return;
    size_t peakK = 0;
    for (size_t k = 1; k < env.db.size(); ++k) {
        if (env.db[k] > env.db[peakK]) peakK = k;
    }
    const double peakDb = env.db[peakK];
    m.envelopePeakDb = peakDb;
    m.envelopePeakTime = std::min(env.time(peakK), m.duration);
    if (peakDb <= kFloorDb) {
        m.tailEnd = "end";
        return;
    }

    size_t onsetK = peakK;
    for (size_t k = 0; k <= peakK; ++k) {
        if (env.db[k] >= peakDb - 30.0) { onsetK = k; break; }
    }
    m.onsetTime = env.time(onsetK);
    m.attackTime = m.envelopePeakTime - m.onsetTime;

    std::vector<double> sorted;
    sorted.reserve(env.db.size());
    for (double d : env.db) {
        if (d > kFloorDb) sorted.push_back(d);
    }
    if (!sorted.empty()) {
        // A floor is a plateau: the frames after the peak within 3 dB of the
        // 10th percentile must not be the end of a decay still under way
        // (a clean decay cut off by the clip's end also has a quiet 10th
        // percentile, but its quiet frames fall steadily).
        const size_t idx = sorted.size() / 10;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(idx), sorted.end());
        const double floorDb = sorted[idx];
        if (floorDb <= peakDb - 20.0) {
            std::vector<double> xs, ys;
            for (size_t k = peakK; k < env.db.size(); ++k) {
                if (env.db[k] > kFloorDb && env.db[k] <= floorDb + 3.0) {
                    xs.push_back(env.time(k));
                    ys.push_back(env.db[k]);
                }
            }
            if (xs.size() >= 4 && slope(xs, ys) > -3.0) m.noiseFloorDb = floorDb;
        }
    }

    double threshold = peakDb + std::min(-1.0, static_cast<double>(opts.tailFloorDb));
    std::string reason = "floor";
    if (!std::isnan(m.noiseFloorDb) && m.noiseFloorDb + 6.0 > threshold) {
        threshold = m.noiseFloorDb + 6.0;
        reason = "noise";
    }
    size_t lastAbove = peakK;
    for (size_t k = env.db.size(); k-- > peakK;) {
        if (env.db[k] >= threshold) { lastAbove = k; break; }
    }
    if (lastAbove + 1 >= env.db.size()) {
        m.tailEnd = "end";
        m.tailTime = m.duration - m.envelopePeakTime;
    } else {
        m.tailEnd = reason;
        m.tailTime = std::min(m.duration, env.time(lastAbove + 1)) - m.envelopePeakTime;
    }
    m.tailTime = std::max(0.0, m.tailTime);

    // Schroeder backward integration from the envelope peak to the tail end,
    // fitted from -5 dB down to 35 dB (or 60% of the tail's depth, when the
    // noise floor cut it short; no estimate below a 10 dB span).
    const size_t n = clip.samples.size();
    const size_t s0 = std::min(n, peakK * static_cast<size_t>(env.hop));
    const size_t s1 = std::min(n, static_cast<size_t>(std::lround((m.envelopePeakTime + m.tailTime) *
                                                                  clip.sampleRate)));
    if (s1 <= s0 + 2) return;
    std::vector<double> edc(s1 - s0 + 1, 0.0);
    for (size_t i = s1; i-- > s0;) {
        const double s = clip.samples[i];
        edc[i - s0] = edc[i - s0 + 1] + s * s;
    }
    if (!(edc[0] > 0.0)) return;
    const double depth = std::min(35.0, 0.6 * (peakDb - threshold));
    if (depth < 15.0) return;
    std::vector<double> xs, ys;
    for (size_t i = 0; i < edc.size(); i += static_cast<size_t>(env.hop)) {
        const double d = 10.0 * std::log10(std::max(edc[i], 1e-300) / edc[0]);
        if (d <= -5.0 && d >= -depth) {
            xs.push_back(static_cast<double>(i) / clip.sampleRate);
            ys.push_back(d);
        }
    }
    if (xs.size() < 3) return;
    m.decayRate = -slope(xs, ys);
    if (m.decayRate > 0.5) m.t60 = 60.0 / m.decayRate;
}

// Long-term spectrum and the timeline's spectral half.
void measureSpectrum(const Clip& clip, const TonalAnalysis& ta, const MeasureOptions& opts,
                     Measurement& m) {
    const int nfft = defaultFftSize(clip.sampleRate);
    const Stft s = computeStft(clip, nfft, nfft / 4);
    std::vector<double> frameSum(s.frames, 0.0);
    double loudest = 0.0;
    for (int f = 0; f < s.frames; ++f) {
        const float* p = s.frame(f);
        for (int k = 0; k < s.bins; ++k) frameSum[f] += p[k];
        loudest = std::max(loudest, frameSum[f]);
    }
    std::vector<double> ltas(s.bins, 0.0);
    for (int f = 0; f < s.frames; ++f) {
        if (!(frameSum[f] >= loudest * 1e-6) || !(loudest > 0.0)) continue;  // within 60 dB
        const float* p = s.frame(f);
        for (int k = 0; k < s.bins; ++k) ltas[k] += p[k];
    }
    m.centroidHz = spectralCentroid(ltas.data(), s.bins, s.binHz);
    m.flatness = spectralFlatness(ltas.data(), s.bins, s.binHz);

    const int slices = std::clamp(opts.slices, 1, 1024);
    const double sliceDur = m.duration / slices;
    m.timeline.resize(slices);
    std::vector<double> acc(s.bins);
    for (int i = 0; i < slices; ++i) {
        Slice& sl = m.timeline[i];
        sl.time = i * sliceDur;
        sl.duration = sliceDur;
        const double t0 = sl.time, t1 = sl.time + sliceDur;
        const size_t a = std::min(clip.samples.size(), static_cast<size_t>(std::lround(t0 * clip.sampleRate)));
        const size_t b = std::min(clip.samples.size(), static_cast<size_t>(std::lround(t1 * clip.sampleRate)));
        double ss = 0.0;
        for (size_t j = a; j < b; ++j) ss += static_cast<double>(clip.samples[j]) * clip.samples[j];
        sl.rmsDb = b > a ? powerDb(ss / static_cast<double>(b - a)) : kFloorDb;
        std::fill(acc.begin(), acc.end(), 0.0);
        bool any = false;
        for (int f = 0; f < s.frames; ++f) {
            const double t = s.time(f);
            if (t < t0 || t >= t1 || (i == slices - 1 && t > m.duration)) continue;
            const float* p = s.frame(f);
            for (int k = 0; k < s.bins; ++k) acc[k] += p[k];
            any = true;
        }
        if (any && sl.rmsDb > kFloorDb) {
            sl.centroidHz = spectralCentroid(acc.data(), s.bins, s.binHz);
            sl.flatness = spectralFlatness(acc.data(), s.bins, s.binHz);
        }
        double e = 0.0, tonal = 0.0;
        for (int f = 0; f < ta.frames; ++f) {
            const double t = ta.time(f);
            if (t < t0 || t >= t1) continue;
            e += ta.frameEnergy[f];
            tonal += ta.frameTonal[f];
        }
        sl.tonality = e > 0.0 ? tonal / e : 0.0;
    }
}

Partial describeTrack(const Track& t, double hopSec, double totalEnergy) {
    Partial p;
    double wsum = 0.0, fsum = 0.0;
    size_t peak = 0;
    for (size_t i = 0; i < t.points.size(); ++i) {
        const TrackPoint& tp = t.points[i];
        wsum += tp.energy;
        fsum += tp.energy * tp.freqHz;
        if (tp.db > t.points[peak].db) peak = i;
    }
    p.freqHz = wsum > 0.0 ? fsum / wsum : t.points.front().freqHz;
    p.peakDb = t.points[peak].db;
    p.startTime = t.points.front().frame * hopSec;
    p.peakTime = t.points[peak].frame * hopSec;
    p.endTime = t.points.back().frame * hopSec;
    size_t lastRing = peak;
    for (size_t i = peak; i < t.points.size(); ++i) {
        if (t.points[i].db >= p.peakDb - 60.0) lastRing = i;
    }
    p.ringTime = (t.points[lastRing].frame - t.points[peak].frame) * hopSec;
    std::vector<double> xs, ys;
    for (size_t i = peak; i <= lastRing; ++i) {
        xs.push_back(t.points[i].frame * hopSec);
        ys.push_back(t.points[i].db);
    }
    if (xs.size() >= 3) p.decayRate = std::max(0.0, -slope(xs, ys));
    if (p.decayRate > 0.5) p.t60 = 60.0 / p.decayRate;
    double var = 0.0;
    for (const TrackPoint& tp : t.points) {
        const double c = 1200.0 * std::log2(tp.freqHz / p.freqHz);
        var += tp.energy * c * c;
    }
    p.stabilityCents = wsum > 0.0 ? std::sqrt(var / wsum) : 0.0;
    p.energyShare = totalEnergy > 0.0 ? t.energy / totalEnergy : 0.0;
    p.ratio = NAN;
    return p;
}

struct Cluster {
    double freqHz = 0;
    double energy = 0;
};

// The fundamental that best explains the clusters as a harmonic series, and
// how far they are from one. Candidates are f/k for the three most
// energetic clusters and k = 1..6 (>= 20 Hz); each costs the amplitude-
// weighted mean distance of every cluster's harmonic number from the nearest
// integer (0..0.5), plus 0.05 per step of k: a subharmonic that fits exactly
// as well loses to the higher candidate, and one that fits only because a
// small integer ratio happens to lie near an inharmonic one (a free bar's
// 2.756 ~ 11/4) needs a much better fit to win. inharmonicity = 2 x that
// distance for the winner.
void fitHarmonics(const std::vector<Cluster>& clusters, Ringing& r) {
    if (clusters.empty()) return;
    if (clusters.size() == 1) {
        r.f0Hz = clusters[0].freqHz;
        r.inharmonicity = 0.0;
        return;
    }
    double bestCost = 1e9, bestDev = 0.5, bestF0 = clusters[0].freqHz;
    const size_t seeds = std::min<size_t>(3, clusters.size());
    for (size_t s = 0; s < seeds; ++s) {
        for (int k = 1; k <= 6; ++k) {
            const double f0 = clusters[s].freqHz / k;
            if (f0 < 20.0) break;
            double wsum = 0.0, dsum = 0.0;
            for (const Cluster& c : clusters) {
                const double w = std::sqrt(c.energy);
                const double h = c.freqHz / f0;
                const double d = std::round(h) < 1.0 ? 0.5 : std::fabs(h - std::round(h));
                wsum += w;
                dsum += w * d;
            }
            const double dev = wsum > 0.0 ? dsum / wsum : 0.5;
            const double cost = dev + 0.05 * (k - 1);
            if (cost < bestCost - 1e-12) {
                bestCost = cost;
                bestDev = dev;
                bestF0 = f0;
            }
        }
    }
    r.f0Hz = bestF0;
    r.inharmonicity = std::clamp(2.0 * bestDev, 0.0, 1.0);
}

void measureRinging(const TonalAnalysis& ta, Measurement& m, int maxPartials) {
    const double total = ta.totalEnergy;
    m.tonality = total > 0.0 ? std::clamp(ta.tonalEnergy / total, 0.0, 1.0) : 0.0;
    std::vector<Partial> all;
    all.reserve(ta.tracks.size());
    for (const Track& t : ta.tracks) all.push_back(describeTrack(t, ta.hopSec, total));

    Ringing& r = m.ringing;
    if (all.empty()) {
        r.f0Hz = NAN;
        return;
    }
    double strongestDb = kFloorDb;
    for (const Partial& p : all) strongestDb = std::max(strongestDb, p.peakDb);
    std::vector<Cluster> clusters;
    double ringNum = 0.0, ringDen = 0.0;
    for (size_t i = 0; i < all.size(); ++i) {
        const Partial& p = all[i];
        if (p.peakDb < strongestDb - 30.0) continue;
        const double e = ta.tracks[i].energy;
        ringNum += e * p.ringTime;
        ringDen += e;
        Cluster* hit = nullptr;
        for (Cluster& c : clusters) {
            if (std::fabs(c.freqHz - p.freqHz) <= std::max(1.5 * ta.binHz, 0.015 * c.freqHz)) {
                hit = &c;
                break;
            }
        }
        if (hit) {
            hit->energy += e;
        } else {
            clusters.push_back({p.freqHz, e});
        }
    }
    std::sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) {
        return a.energy != b.energy ? a.energy > b.energy : a.freqHz < b.freqHz;
    });
    r.count = static_cast<int>(clusters.size());
    double top3 = 0.0;
    for (size_t i = 0; i < clusters.size() && i < 3; ++i) top3 += clusters[i].energy;
    r.sparsity = ta.tonalEnergy > 0.0 ? std::clamp(top3 / ta.tonalEnergy, 0.0, 1.0) : 0.0;
    r.strongestRingTime = all.front().ringTime;
    r.weightedRingTime = ringDen > 0.0 ? ringNum / ringDen : 0.0;
    fitHarmonics(clusters, r);
    r.ringScore = m.tonality * r.sparsity * std::min(1.0, r.weightedRingTime / 0.5);

    const size_t keep = std::min(all.size(), static_cast<size_t>(std::max(0, maxPartials)));
    m.partials.assign(all.begin(), all.begin() + static_cast<long>(keep));
    for (Partial& p : m.partials) {
        p.ratio = std::isnan(r.f0Hz) || r.f0Hz <= 0.0 ? NAN : p.freqHz / r.f0Hz;
    }
}

} // namespace

Measurement measure(const Clip& clip, const MeasureOptions& opts) {
    Measurement m;
    m.sampleRate = clip.sampleRate;
    m.channels = clip.channels;
    m.duration = clip.duration();
    m.ringing.f0Hz = NAN;
    if (clip.samples.empty() || clip.sampleRate <= 0) return m;

    double peak = 0.0, ss = 0.0;
    size_t peakI = 0;
    for (size_t i = 0; i < clip.samples.size(); ++i) {
        const double a = std::fabs(static_cast<double>(clip.samples[i]));
        if (a > peak) { peak = a; peakI = i; }
        ss += a * a;
    }
    m.peakDb = ampDb(peak);
    m.peakTime = static_cast<double>(peakI) / clip.sampleRate;
    m.rmsDb = powerDb(ss / static_cast<double>(clip.samples.size()));
    const Loudness loud = clipLoudness(clip);
    m.lufs = loud.lufs;
    m.lufsShort = loud.lufsShort;
    m.loudness = loud.loudness();

    const Envelope env = computeEnvelope(clip);
    measureTiming(clip, env, opts, m);
    const TonalAnalysis ta = analyzeTonal(clip);
    measureSpectrum(clip, ta, opts, m);
    measureRinging(ta, m, opts.maxPartials);
    return m;
}

} // namespace broaudio::ear
