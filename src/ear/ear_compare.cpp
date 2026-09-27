// compare(): how far a clip is from a reference. See ear.h for the
// definitions; the steps are
//   1. both clips at the lower of the two rates (broaudio's resampler);
//   2. each loudness-normalised to -23 LUFS (RMS to -23 dBFS when either
//      clip's LUFS is undefined), so level never counts;
//   3. onsets aligned (first envelope frame within 30 dB of the peak), then
//      refined over +-maxShift to the lag with the smallest envelope distance;
//   4. envelope, mel-spectrum and tonality distances over the union of both
//      lengths, the shorter clip padded with silence.

#include "ear_internal.h"

#include <algorithm>
#include <cmath>

namespace broaudio::ear {

using namespace detail;

namespace {

constexpr double kEnvFloor = -60.0;
constexpr int kMelBands = 40;

size_t onsetFrame(const Envelope& e) {
    if (e.db.empty()) return 0;
    const double peak = *std::max_element(e.db.begin(), e.db.end());
    for (size_t k = 0; k < e.db.size(); ++k) {
        if (e.db[k] >= peak - 30.0) return k;
    }
    return 0;
}

// Mean |difference| of the two peak-relative envelopes (floored at -60 dB)
// with frame k of the reference against frame k + lag of the clip, over the
// frames where either is above the floor.
double envelopeDistance(const std::vector<double>& a, const std::vector<double>& b, long lag) {
    const long na = static_cast<long>(a.size()), nb = static_cast<long>(b.size());
    const long k0 = std::min(0L, -lag), k1 = std::max(nb, na - lag);
    double sum = 0.0;
    long count = 0;
    for (long k = k0; k < k1; ++k) {
        const long ia = k + lag;
        const double va = (ia >= 0 && ia < na) ? a[ia] : kEnvFloor;
        const double vb = (k >= 0 && k < nb) ? b[k] : kEnvFloor;
        if (va <= kEnvFloor && vb <= kEnvFloor) continue;
        sum += std::fabs(va - vb);
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}

std::vector<double> relativeEnvelope(const Envelope& e) {
    std::vector<double> r(e.db.size(), kEnvFloor);
    if (e.db.empty()) return r;
    const double peak = *std::max_element(e.db.begin(), e.db.end());
    for (size_t k = 0; k < e.db.size(); ++k) r[k] = std::max(kEnvFloor, e.db[k] - peak);
    return r;
}

// Triangular mel filters (40 bands, 40 Hz .. min(16 kHz, Nyquist)); a band
// narrower than two bins is widened to +-1 bin so it is never empty.
struct MelBank {
    std::vector<std::vector<std::pair<int, float>>> bands;
};

MelBank makeMelBank(int bins, double binHz) {
    MelBank mb;
    const double nyq = (bins - 1) * binHz;
    const double lo = hzToMel(40.0), hi = hzToMel(std::min(16000.0, nyq));
    std::vector<double> edges(kMelBands + 2);
    for (int i = 0; i < kMelBands + 2; ++i) edges[i] = melToHz(lo + (hi - lo) * i / (kMelBands + 1));
    mb.bands.resize(kMelBands);
    for (int b = 0; b < kMelBands; ++b) {
        double l = edges[b], c = edges[b + 1], h = edges[b + 2];
        if (h - l < 2.0 * binHz) {
            l = c - binHz;
            h = c + binHz;
        }
        for (int k = std::max(0, static_cast<int>(std::floor(l / binHz)));
             k <= std::min(bins - 1, static_cast<int>(std::ceil(h / binHz))); ++k) {
            const double f = k * binHz;
            double w = 0.0;
            if (f > l && f <= c) w = (f - l) / (c - l);
            else if (f > c && f < h) w = (h - f) / (h - c);
            if (w > 0.0) mb.bands[b].push_back({k, static_cast<float>(w)});
        }
    }
    return mb;
}

// Frames x bands of mel power.
std::vector<double> melFrames(const Stft& s, const MelBank& mb) {
    std::vector<double> out(static_cast<size_t>(s.frames) * kMelBands, 0.0);
    for (int f = 0; f < s.frames; ++f) {
        const float* p = s.frame(f);
        for (int b = 0; b < kMelBands; ++b) {
            double v = 0.0;
            for (const auto& [k, w] : mb.bands[b]) v += w * p[k];
            out[static_cast<size_t>(f) * kMelBands + b] = v;
        }
    }
    return out;
}

void scale(Clip& c, double gain) {
    for (float& s : c.samples) s = static_cast<float>(s * gain);
}

double meanSquare(const Clip& c) {
    double ss = 0.0;
    for (float s : c.samples) ss += static_cast<double>(s) * s;
    return c.samples.empty() ? 0.0 : ss / static_cast<double>(c.samples.size());
}

} // namespace

Comparison compare(const Clip& clip, const Clip& reference, const CompareOptions& opts) {
    Comparison r;
    if (clip.sampleRate <= 0 || reference.sampleRate <= 0) {
        r.score = r.envelope = r.spectrum = r.tonality = NAN;
        return r;
    }
    const int rate = std::min(clip.sampleRate, reference.sampleRate);
    r.sampleRate = rate;
    Clip a = resampled(clip, rate);
    Clip b = resampled(reference, rate);

    // 2. Loudness.
    // One scale for both: the blend weight of the shorter clip.
    const double shorter = std::min(a.duration(), b.duration());
    const double w = loudnessLufsWeight(shorter);
    const double la = clipLoudness(a).at(shorter), lb = clipLoudness(b).at(shorter);
    const double msA = meanSquare(a), msB = meanSquare(b);
    r.loudnessScale = w >= 1.0 ? "lufs" : w <= 0.0 ? "lufsShort" : "blend";
    if (!std::isnan(la) && !std::isnan(lb)) {
        r.loudnessDiffDb = la - lb;
        scale(a, std::pow(10.0, (-23.0 - la) / 20.0));
        scale(b, std::pow(10.0, (-23.0 - lb) / 20.0));
    } else {
        r.loudnessScale = "rms";
        r.loudnessDiffDb = powerDb(msA) - powerDb(msB);
        if (msA > 1e-20) scale(a, std::pow(10.0, -23.0 / 20.0) / std::sqrt(msA));
        if (msB > 1e-20) scale(b, std::pow(10.0, -23.0 / 20.0) / std::sqrt(msB));
    }

    // 3. Alignment.
    const Envelope ea = computeEnvelope(a), eb = computeEnvelope(b);
    const std::vector<double> ra = relativeEnvelope(ea), rb = relativeEnvelope(eb);
    long lag = 0;
    if (opts.align) {
        const long base = static_cast<long>(onsetFrame(ea)) - static_cast<long>(onsetFrame(eb));
        const long reach = std::max(0L, static_cast<long>(std::lround(opts.maxShift / ea.hopSec)));
        double best = 1e300;
        // Nearest the onset estimate first, so a tie keeps the smaller move.
        for (long d = 0; d <= reach; ++d) {
            for (long sgn : {1L, -1L}) {
                if (d == 0 && sgn < 0) continue;
                const long l = base + sgn * d;
                const double dist = envelopeDistance(ra, rb, l);
                if (dist < best - 1e-12) {
                    best = dist;
                    lag = l;
                }
            }
        }
    }
    r.offsetTime = lag * ea.hopSec;
    r.envelopeDb = envelopeDistance(ra, rb, lag);
    const long shift = lag * ea.hop;
    if (shift > 0) {
        a.samples.erase(a.samples.begin(),
                        a.samples.begin() + std::min<long>(shift, static_cast<long>(a.samples.size())));
    } else if (shift < 0) {
        a.samples.insert(a.samples.begin(), static_cast<size_t>(-shift), 0.0f);
    }

    // 4. Spectrum.
    const int nfft = defaultFftSize(rate);
    const Stft sa = computeStft(a, nfft, nfft / 4), sb = computeStft(b, nfft, nfft / 4);
    const MelBank mb = makeMelBank(sa.bins, sa.binHz);
    const std::vector<double> ma = melFrames(sa, mb), mbands = melFrames(sb, mb);
    const int frames = std::max(sa.frames, sb.frames);
    auto bandAt = [](const std::vector<double>& m, int nf, int f, int b) {
        return f < nf ? m[static_cast<size_t>(f) * kMelBands + b] : 0.0;
    };
    double top = 1e-30;
    for (double v : ma) top = std::max(top, v);
    for (double v : mbands) top = std::max(top, v);
    const double floorDb = 10.0 * std::log10(top) - 80.0;
    std::vector<double> totA(frames, 0.0), totB(frames, 0.0);
    double loudA = 0.0, loudB = 0.0;
    std::vector<double> ltasA(kMelBands, 0.0), ltasB(kMelBands, 0.0);
    for (int f = 0; f < frames; ++f) {
        for (int b = 0; b < kMelBands; ++b) {
            const double va = bandAt(ma, sa.frames, f, b), vb = bandAt(mbands, sb.frames, f, b);
            totA[f] += va;
            totB[f] += vb;
            ltasA[b] += va;
            ltasB[b] += vb;
        }
        loudA = std::max(loudA, totA[f]);
        loudB = std::max(loudB, totB[f]);
    }
    double specSum = 0.0;
    int specCount = 0;
    for (int f = 0; f < frames; ++f) {
        const bool active = (loudA > 0.0 && totA[f] >= loudA * 1e-6) || (loudB > 0.0 && totB[f] >= loudB * 1e-6);
        if (!active) continue;
        double d = 0.0;
        for (int b = 0; b < kMelBands; ++b) {
            const double da = std::max(floorDb, powerDb(bandAt(ma, sa.frames, f, b) + 1e-30));
            const double db = std::max(floorDb, powerDb(bandAt(mbands, sb.frames, f, b) + 1e-30));
            d += std::fabs(da - db);
        }
        specSum += d / kMelBands;
        ++specCount;
    }
    r.spectrogramDb = specCount > 0 ? specSum / specCount : 0.0;

    double sumA = 0.0, sumB = 0.0, maxA = 0.0, maxB = 0.0;
    for (int b = 0; b < kMelBands; ++b) { sumA += ltasA[b]; sumB += ltasB[b]; }
    for (int b = 0; b < kMelBands; ++b) {
        ltasA[b] = sumA > 0.0 ? ltasA[b] / sumA : 0.0;
        ltasB[b] = sumB > 0.0 ? ltasB[b] / sumB : 0.0;
        maxA = std::max(maxA, ltasA[b]);
        maxB = std::max(maxB, ltasB[b]);
    }
    double ltasSum = 0.0;
    int ltasCount = 0;
    const double relFloorA = maxA * 1e-6, relFloorB = maxB * 1e-6;  // 60 dB
    for (int b = 0; b < kMelBands; ++b) {
        if (ltasA[b] < relFloorA && ltasB[b] < relFloorB) continue;
        const double da = 10.0 * std::log10(std::max(ltasA[b], std::max(relFloorA, 1e-30)));
        const double db = 10.0 * std::log10(std::max(ltasB[b], std::max(relFloorB, 1e-30)));
        ltasSum += std::fabs(da - db);
        ++ltasCount;
    }
    r.ltasDb = ltasCount > 0 ? ltasSum / ltasCount : 0.0;

    // Tonality, from each aligned clip's own report.
    MeasureOptions mo;
    mo.slices = 1;
    mo.maxPartials = 0;
    const Measurement mA = measure(a, mo), mB = measure(b, mo);
    r.centroidRatio = mB.centroidHz > 0.0 ? mA.centroidHz / mB.centroidHz : 1.0;
    r.tonalityDiff = mA.tonality - mB.tonality;
    r.ringTimeRatio = (mA.ringing.weightedRingTime + 0.05) / (mB.ringing.weightedRingTime + 0.05);
    r.inharmonicityDiff = mA.ringing.inharmonicity - mB.ringing.inharmonicity;
    r.durationDiff = (mA.envelopePeakTime + mA.tailTime) - (mB.envelopePeakTime + mB.tailTime);

    r.envelope = r.envelopeDb / 30.0;
    r.spectrum = (r.spectrogramDb + r.ltasDb) / 2.0 / 20.0;
    r.tonality = 0.5 * std::fabs(r.tonalityDiff) +
                 0.3 * std::min(1.0, std::fabs(std::log2(r.ringTimeRatio)) / 2.0) +
                 0.2 * std::fabs(r.inharmonicityDiff);
    r.score = opts.envelopeWeight * r.envelope + opts.spectrumWeight * r.spectrum +
              opts.tonalityWeight * r.tonality;
    return r;
}

} // namespace broaudio::ear
