// The ear's signal stages: clips, the envelope, the STFT, loudness and the
// per-spectrum descriptors. See ear.h for the contract.

#include "ear_internal.h"

#include "broaudio/dsp/fft.h"
#include "broaudio/dsp/resampler.h"
#include "broaudio/io/audio_file.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace broaudio::ear {

Clip monoClip(const float* interleaved, size_t frames, int channels, int sampleRate) {
    Clip c;
    c.sampleRate = sampleRate;
    c.channels = channels > 0 ? channels : 1;
    c.samples.resize(frames);
    if (!interleaved || frames == 0) return c;
    if (c.channels == 1) {
        std::copy(interleaved, interleaved + frames, c.samples.begin());
        return c;
    }
    const float inv = 1.0f / static_cast<float>(c.channels);
    for (size_t i = 0; i < frames; ++i) {
        float s = 0.0f;
        const float* f = interleaved + i * static_cast<size_t>(c.channels);
        for (int ch = 0; ch < c.channels; ++ch) s += f[ch];
        c.samples[i] = s * inv;
    }
    return c;
}

bool loadClip(const std::string& path, Clip& out, std::string* error) {
    AudioFileData data = loadAudioFile(path.c_str());
    if (!data.valid()) {
        if (error) *error = data.error.empty() ? "could not decode " + path : path + ": " + data.error;
        return false;
    }
    out = monoClip(data.samples.data(), static_cast<size_t>(data.numFrames), data.channels,
                   data.sampleRate);
    return true;
}

Clip resampled(const Clip& clip, int rate) {
    if (rate <= 0 || clip.sampleRate == rate || clip.samples.empty()) {
        Clip c = clip;
        if (rate > 0) c.sampleRate = rate;
        return c;
    }
    Clip c;
    c.sampleRate = rate;
    c.channels = clip.channels;
    c.samples = resample(clip.samples.data(), static_cast<int>(clip.samples.size()), 1,
                         clip.sampleRate, rate);
    return c;
}

namespace detail {

Envelope computeEnvelope(const Clip& clip) {
    Envelope env;
    const int sr = clip.sampleRate > 0 ? clip.sampleRate : 1;
    env.hop = std::max(1, static_cast<int>(std::lround(sr * 0.005)));
    env.hopSec = static_cast<double>(env.hop) / sr;
    const size_t n = clip.samples.size();
    std::vector<double> prefix(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double s = clip.samples[i];
        prefix[i + 1] = prefix[i] + s * s;
    }
    const size_t frames = n / static_cast<size_t>(env.hop) + 1;
    env.db.resize(frames);
    const long long half = env.hop;  // the window is 2 hops wide
    for (size_t k = 0; k < frames; ++k) {
        const long long c = static_cast<long long>(k) * env.hop;
        const long long a = std::max(0LL, c - half);
        const long long b = std::min(static_cast<long long>(n), c + half);
        const double ms = b > a ? (prefix[b] - prefix[a]) / static_cast<double>(2 * half) : 0.0;
        // Squares of tiny floats accumulate rounding: clamp a negative
        // difference of prefix sums to zero.
        env.db[k] = powerDb(std::max(0.0, ms));
    }
    return env;
}

int defaultFftSize(int sampleRate) {
    return std::clamp(nextPow2(static_cast<int>(std::lround(sampleRate * 0.04))), 256, 16384);
}

Stft computeStft(const Clip& clip, int nfft, int hop) {
    Stft s;
    s.nfft = std::max(16, nextPow2(nfft));
    s.hop = std::max(1, hop);
    s.bins = s.nfft / 2 + 1;
    const int sr = clip.sampleRate > 0 ? clip.sampleRate : 1;
    s.binHz = static_cast<double>(sr) / s.nfft;
    s.hopSec = static_cast<double>(s.hop) / sr;
    const long long n = static_cast<long long>(clip.samples.size());
    s.frames = static_cast<int>(n / s.hop) + 1;
    s.power.assign(static_cast<size_t>(s.frames) * s.bins, 0.0f);

    std::vector<float> window(s.nfft);
    for (int i = 0; i < s.nfft; ++i) {
        window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / s.nfft));
    }
    // Scaled so a sine of amplitude A peaks at A^2: (|X| * 2 / sum(w))^2 with
    // sum(w) = nfft/2 for a periodic Hann window.
    const double scale = 16.0 / (static_cast<double>(s.nfft) * s.nfft);
    std::vector<float> re(s.nfft), im(s.nfft);
    const int half = s.nfft / 2;
    for (int f = 0; f < s.frames; ++f) {
        const long long start = static_cast<long long>(f) * s.hop - half;
        for (int i = 0; i < s.nfft; ++i) {
            const long long idx = start + i;
            re[i] = (idx >= 0 && idx < n) ? clip.samples[static_cast<size_t>(idx)] * window[i] : 0.0f;
            im[i] = 0.0f;
        }
        fft(re.data(), im.data(), s.nfft);
        float* out = s.power.data() + static_cast<size_t>(f) * s.bins;
        for (int k = 0; k < s.bins; ++k) {
            out[k] = static_cast<float>((static_cast<double>(re[k]) * re[k] +
                                         static_cast<double>(im[k]) * im[k]) * scale);
        }
    }
    return s;
}

namespace {

// A direct-form-I biquad in double precision.
struct Biquad {
    double b0, b1, b2, a1, a2;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double run(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// The BS.1770 K-weighting pre-filter (a high shelf) and RLB high-pass,
// designed for any rate by the bilinear transform (the libebur128 design).
void kWeighting(int fs, Biquad& shelf, Biquad& hp) {
    const double pi = std::numbers::pi;
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = std::tan(pi * f0 / fs);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    shelf = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
             2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = std::tan(pi * f0 / fs);
    a0 = 1.0 + K / Q + K * K;
    hp = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
}

} // namespace

double integratedLufs(const Clip& clip) {
    const size_t n = clip.samples.size();
    if (n == 0 || clip.sampleRate <= 0) return NAN;
    Biquad shelf{}, hp{};
    kWeighting(clip.sampleRate, shelf, hp);
    std::vector<double> prefix(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double y = hp.run(shelf.run(clip.samples[i]));
        prefix[i + 1] = prefix[i] + y * y;
    }
    const size_t blockLen = static_cast<size_t>(std::lround(clip.sampleRate * 0.4));
    const size_t step = std::max<size_t>(1, static_cast<size_t>(std::lround(clip.sampleRate * 0.1)));
    std::vector<double> z;
    if (n < blockLen) {
        z.push_back(prefix[n] / static_cast<double>(n));
    } else {
        for (size_t start = 0; start + blockLen <= n; start += step) {
            z.push_back((prefix[start + blockLen] - prefix[start]) / static_cast<double>(blockLen));
        }
    }
    auto loud = [](double ms) { return -0.691 + 10.0 * std::log10(ms); };
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0.0;
    int count = 0;
    for (double v : z) {
        if (v > absGate) { sum += v; ++count; }
    }
    if (count == 0) return NAN;
    const double relGate = std::pow(10.0, (loud(sum / count) - 10.0 + 0.691) / 10.0);
    double sum2 = 0.0;
    int count2 = 0;
    for (double v : z) {
        if (v > absGate && v > relGate) { sum2 += v; ++count2; }
    }
    if (count2 == 0) return NAN;
    return loud(sum2 / count2);
}

double spectralCentroid(const double* power, int bins, double binHz) {
    double num = 0.0, den = 0.0;
    for (int k = 1; k < bins; ++k) {
        num += k * binHz * power[k];
        den += power[k];
    }
    return den > 0.0 ? num / den : 0.0;
}

double spectralFlatness(const double* power, int bins, double binHz) {
    const double nyq = (bins - 1) * binHz;
    const int lo = std::max(1, static_cast<int>(std::ceil(30.0 / binHz)));
    const int hi = std::min(bins - 1, static_cast<int>(std::floor(std::min(16000.0, nyq) / binHz)));
    if (hi <= lo) return 0.0;
    double logSum = 0.0, sum = 0.0;
    for (int k = lo; k <= hi; ++k) {
        const double p = power[k] + 1e-20;
        logSum += std::log(p);
        sum += p;
    }
    const int count = hi - lo + 1;
    const double mean = sum / count;
    if (!(mean > 1e-19)) return 0.0;
    return std::clamp(std::exp(logSum / count) / mean, 0.0, 1.0);
}

double slope(const std::vector<double>& x, const std::vector<double>& y) {
    const size_t n = std::min(x.size(), y.size());
    if (n < 2) return 0.0;
    double mx = 0, my = 0;
    for (size_t i = 0; i < n; ++i) { mx += x[i]; my += y[i]; }
    mx /= n;
    my /= n;
    double sxy = 0, sxx = 0;
    for (size_t i = 0; i < n; ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
    }
    return sxx > 0 ? sxy / sxx : 0.0;
}

} // namespace detail
} // namespace broaudio::ear
