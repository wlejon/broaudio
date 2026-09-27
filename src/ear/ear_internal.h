#pragma once

// Shared analysis stages of the ear (include/broaudio/ear/ear.h): the
// envelope, the STFT, loudness and the partial tracker. Internal; the public
// contract is ear.h.

#include "broaudio/ear/ear.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace broaudio::ear::detail {

inline constexpr double kFloorDb = -120.0;

inline double powerDb(double meanSquare) {
    return meanSquare > 1e-12 ? 10.0 * std::log10(meanSquare) : kFloorDb;
}
inline double ampDb(double amplitude) {
    return amplitude > 1e-6 ? 20.0 * std::log10(amplitude) : kFloorDb;
}

inline int nextPow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// The envelope: RMS of a 10 ms window every 5 ms, frame k centred on sample
// k*hop, in dBFS (a full-scale square wave reads 0).
struct Envelope {
    int hop = 1;          // samples
    double hopSec = 0;
    std::vector<double> db;
    double time(size_t k) const { return static_cast<double>(k) * hopSec; }
};
Envelope computeEnvelope(const Clip& clip);

// A Hann-windowed STFT. Frame i is centred on sample i*hop (zero-padded past
// either end); `power` holds frames x bins |X|^2 scaled so a sine of
// amplitude A peaks at A^2 in its nearest bin.
struct Stft {
    int nfft = 0;
    int hop = 0;
    int bins = 0;  // nfft/2 + 1
    int frames = 0;
    double binHz = 0;
    double hopSec = 0;
    std::vector<float> power;
    const float* frame(int i) const { return power.data() + static_cast<size_t>(i) * bins; }
    double time(int i) const { return i * hopSec; }
};
// fftSize 0 picks ~43 ms (nextPow2(0.04 * rate)).
int defaultFftSize(int sampleRate);
Stft computeStft(const Clip& clip, int nfft, int hop);

// BS.1770-4 integrated loudness of the mono clip; NaN when no block passes
// the -70 LUFS absolute gate.
double integratedLufs(const Clip& clip);

// Every loudness of a clip (ear.h's Measurement::lufs / lufsShort /
// loudness), from one K-weighting pass.
struct Loudness {
    double lufs = NAN;
    double lufsShort = NAN;
    double duration = 0;
    // The blend of lufsShort (weight 1 - w) and lufs (w) in dB, w from
    // `duration` (0 under 400 ms, 1 from 800 ms); whichever is defined when
    // the other is not.
    double at(double duration) const;
    double loudness() const { return at(duration); }
};
Loudness clipLoudness(const Clip& clip);
// The blend weight of lufs for a clip `duration` seconds long.
double loudnessLufsWeight(double duration);

// Spectral centroid and flatness of one power spectrum.
double spectralCentroid(const double* power, int bins, double binHz);
double spectralFlatness(const double* power, int bins, double binHz);

// Partial tracking over a long-window STFT (~100 ms, hop 1/8). A track is a
// run of prominent, frequency-continuous spectral peaks.
struct TrackPoint {
    int frame = 0;
    double freqHz = 0;
    double energy = 0;  // the peak's main-lobe energy, amplitude^2 / 2 units (a sine's mean square)
    double db = 0;      // the sinusoid's amplitude, dBFS
};
struct Track {
    std::vector<TrackPoint> points;
    double energy = 0;  // sum over points, times hopSec (so it is comparable across hops)
    double duration(double hopSec) const {
        return points.empty() ? 0.0 : (points.back().frame - points.front().frame) * hopSec;
    }
};
struct TonalAnalysis {
    double hopSec = 0;
    double binHz = 0;
    int frames = 0;
    std::vector<double> frameEnergy;  // all bins, mean-square units
    std::vector<double> frameTonal;   // energy in partials of qualifying tracks
    std::vector<Track> tracks;        // qualifying tracks only (>= 60 ms), strongest first
    double totalEnergy = 0;           // sums over frames, times hopSec
    double tonalEnergy = 0;
    double time(int frame) const { return frame * hopSec; }
};
TonalAnalysis analyzeTonal(const Clip& clip);

// Least-squares slope of y over x.
double slope(const std::vector<double>& x, const std::vector<double>& y);

// Frequency-scale mappings.
inline double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
inline double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

// The 5x7 bitmap font (ear_font.cpp): the rows of `c`, bit 4 = leftmost
// column, or nullptr for a character it lacks (drawn as a blank).
const uint8_t* glyph5x7(char c);

} // namespace broaudio::ear::detail
