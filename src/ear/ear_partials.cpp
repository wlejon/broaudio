// The ear's partial tracker: which spectral peaks are pure, stable
// sinusoids, and how their energy compares with the rest of the clip.
//
// A long window (~100 ms, hop 1/8) resolves partials 2-3 bins apart at any
// rate. Per frame, a bin is a candidate peak when it is a local maximum
// within 80 dB of the frame's loudest bin and at least 15 dB above the median
// of the 49 bins around it. White noise essentially never clears that bar (an
// exponentially distributed periodogram bin exceeds 21.9x its median with
// probability ~3e-10), while a sinusoid's Hann main lobe clears it by the
// side-lobe margin. The peak's frequency is refined by parabolic
// interpolation of the log power over its three bins, and its energy is the
// power of its main lobe (+-2 bins), bins claimed strongest peak first so two
// peaks never count one bin twice.
//
// Tracking is greedy, strongest peak first: a peak continues the nearest
// active track (seen within the last 3 frames) that is within max(1 bin,
// 2.5%) of the track's last frequency, or starts a new one. A track counts as
// a partial when it lasts at least 60 ms and 4 frames; the energy of those
// tracks is the clip's tonal energy.

#include "ear_internal.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace broaudio::ear::detail {

namespace {

struct Peak {
    int bin = 0;
    double freqHz = 0;
    double energy = 0;
    double db = 0;
    float power = 0;
};

struct ActiveTrack {
    size_t index = 0;  // into the track list
    int lastFrame = 0;
    double lastFreq = 0;
    int matchedFrame = -1;
};

constexpr double kProminence = 31.6227766;  // 15 dB
constexpr int kMedianHalfWidth = 24;
constexpr int kMaxPeaksPerFrame = 80;
constexpr int kMaxGapFrames = 3;
constexpr double kMinTrackSeconds = 0.06;
constexpr int kMinTrackPoints = 4;

// The prominent peaks of one power spectrum, strongest first, with their
// main-lobe energies claimed.
void findPeaks(const float* p, int bins, double binHz, std::vector<Peak>& out,
               std::vector<uint8_t>& claimed, std::vector<float>& scratch) {
    out.clear();
    float frameMax = 0.0f;
    for (int k = 0; k < bins; ++k) frameMax = std::max(frameMax, p[k]);
    if (!(frameMax > 0.0f)) return;
    const float floorPower = frameMax * 1e-8f;
    const int kLo = std::max(2, static_cast<int>(std::ceil(20.0 / binHz)));
    for (int k = kLo; k < bins - 2; ++k) {
        if (!(p[k] > p[k - 1] && p[k] >= p[k + 1] && p[k] >= floorPower)) continue;
        const int a = std::max(0, k - kMedianHalfWidth);
        const int b = std::min(bins - 1, k + kMedianHalfWidth);
        scratch.assign(p + a, p + b + 1);
        auto mid = scratch.begin() + static_cast<long>(scratch.size() / 2);
        std::nth_element(scratch.begin(), mid, scratch.end());
        if (!(p[k] >= *mid * kProminence)) continue;
        Peak pk;
        pk.bin = k;
        pk.power = p[k];
        const double l = std::log(p[k - 1] + 1e-30);
        const double c = std::log(p[k] + 1e-30);
        const double r = std::log(p[k + 1] + 1e-30);
        const double den = l - 2.0 * c + r;
        const double delta = den < 0.0 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
        pk.freqHz = (k + delta) * binHz;
        out.push_back(pk);
    }
    std::sort(out.begin(), out.end(), [](const Peak& x, const Peak& y) {
        return x.power != y.power ? x.power > y.power : x.bin < y.bin;
    });
    if (out.size() > static_cast<size_t>(kMaxPeaksPerFrame)) out.resize(kMaxPeaksPerFrame);
    claimed.assign(static_cast<size_t>(bins), 0);
    for (Peak& pk : out) {
        double lobe = 0.0;
        for (int j = pk.bin - 2; j <= pk.bin + 2; ++j) {
            if (j < 0 || j >= bins || claimed[j]) continue;
            claimed[j] = 1;
            lobe += p[j];
        }
        // Scaled power sums to 3x the mean square over the one-sided
        // spectrum (Hann), and a sine's mean square is A^2 / 2.
        pk.energy = lobe / 3.0;
        pk.db = ampDb(std::sqrt(2.0 * pk.energy));
    }
}

} // namespace

TonalAnalysis analyzeTonal(const Clip& clip) {
    TonalAnalysis ta;
    const int sr = clip.sampleRate > 0 ? clip.sampleRate : 1;
    const int nfft = std::clamp(nextPow2(static_cast<int>(std::lround(sr * 0.1))), 512, 32768);
    const Stft stft = computeStft(clip, nfft, nfft / 8);
    ta.hopSec = stft.hopSec;
    ta.binHz = stft.binHz;
    ta.frames = stft.frames;
    ta.frameEnergy.assign(stft.frames, 0.0);
    ta.frameTonal.assign(stft.frames, 0.0);

    double loudest = 0.0;
    for (int f = 0; f < stft.frames; ++f) {
        const float* p = stft.frame(f);
        double sum = 0.0;
        for (int k = 0; k < stft.bins; ++k) sum += p[k];
        ta.frameEnergy[f] = sum / 3.0;
        loudest = std::max(loudest, ta.frameEnergy[f]);
    }

    std::vector<Track> tracks;
    std::vector<ActiveTrack> active;
    std::vector<Peak> peaks;
    std::vector<uint8_t> claimed;
    std::vector<float> scratch;
    for (int f = 0; f < stft.frames; ++f) {
        // Drop tracks that have not been continued for too long.
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [f](const ActiveTrack& t) { return f - t.lastFrame > kMaxGapFrames; }),
                     active.end());
        if (!(loudest > 0.0) || ta.frameEnergy[f] < loudest * 1e-10) continue;
        findPeaks(stft.frame(f), stft.bins, stft.binHz, peaks, claimed, scratch);
        for (const Peak& pk : peaks) {
            const double tol = std::max(stft.binHz, 0.025 * pk.freqHz);
            ActiveTrack* best = nullptr;
            double bestDist = tol;
            for (ActiveTrack& t : active) {
                if (t.matchedFrame == f) continue;
                const double d = std::fabs(pk.freqHz - t.lastFreq);
                if (d <= bestDist) {
                    bestDist = d;
                    best = &t;
                }
            }
            TrackPoint tp{f, pk.freqHz, pk.energy, pk.db};
            if (best) {
                tracks[best->index].points.push_back(tp);
                best->lastFrame = f;
                best->lastFreq = pk.freqHz;
                best->matchedFrame = f;
            } else {
                tracks.emplace_back();
                tracks.back().points.push_back(tp);
                active.push_back({tracks.size() - 1, f, pk.freqHz, f});
            }
        }
    }

    for (Track& t : tracks) {
        if (static_cast<int>(t.points.size()) < kMinTrackPoints ||
            t.duration(ta.hopSec) < kMinTrackSeconds - 1e-9) {
            continue;
        }
        double e = 0.0;
        for (const TrackPoint& tp : t.points) {
            e += tp.energy;
            ta.frameTonal[tp.frame] += tp.energy;
        }
        t.energy = e * ta.hopSec;
        ta.tracks.push_back(std::move(t));
    }
    // Deterministic order: energy, then start frame, then frequency.
    std::sort(ta.tracks.begin(), ta.tracks.end(), [](const Track& a, const Track& b) {
        if (a.energy != b.energy) return a.energy > b.energy;
        if (a.points.front().frame != b.points.front().frame) {
            return a.points.front().frame < b.points.front().frame;
        }
        return a.points.front().freqHz < b.points.front().freqHz;
    });
    for (int f = 0; f < stft.frames; ++f) {
        ta.frameTonal[f] = std::min(ta.frameTonal[f], ta.frameEnergy[f]);
        ta.totalEnergy += ta.frameEnergy[f] * ta.hopSec;
        ta.tonalEnergy += ta.frameTonal[f] * ta.hopSec;
    }
    return ta;
}

} // namespace broaudio::ear::detail
