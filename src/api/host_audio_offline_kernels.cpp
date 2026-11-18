// Each node kind's processing for one render quantum of an offline render
// (host_audio_offline.h), as the Web Audio specification defines it and
// Chromium computes it where the specification leaves room: band-limited
// oscillator tables (PeriodicWave), the cookbook biquads, a linearly
// interpolated delay line, the equal-power panners, the WaveShaper curve, a
// normalized convolution and a soft-knee compressor.

#include "host_audio_offline.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <mutex>
#include <numbers>

namespace broaudio::api {

// What a node keeps between quanta. One type for every kind; each uses its
// own fields.
struct OfflineNodeState {
    // Oscillator: the read position in the wave table (table samples).
    double phase = 0.0;
    std::shared_ptr<const struct WaveTables> tables;
    // Buffer source: the playhead (buffer frames), content played (buffer
    // frames, for `duration`), whether it ran off its end.
    double playhead = 0.0;
    double played = 0.0;
    bool begun = false;
    bool finished = false;
    // Biquad (per channel x1 x2 y1 y2) and IIR histories.
    std::vector<double> mem;
    // Delay line: a ring per channel and its write position.
    std::vector<std::vector<float>> line;
    int64_t written = 0;
    // Convolver: the impulse's spectra per channel and partition, and the
    // input spectra per input channel, newest first.
    std::vector<std::vector<std::vector<std::complex<double>>>> irSpectra;
    std::vector<std::vector<std::vector<std::complex<double>>>> inputSpectra;
    std::vector<std::vector<float>> lastInput;
    int convChannelsIn = 0;
    // Compressor: the lookahead line, the detector's envelope and the gain.
    std::vector<std::vector<float>> lookahead;
    int lookaheadPos = 0;
    double envelope = 0.0;
    double gainDb = 0.0;
};

namespace {

OfflineNodeState& stateOf(HostAudioNode& n) {
    if (!n.offlineState) n.offlineState = std::make_shared<OfflineNodeState>();
    return *n.offlineState;
}

// ---- FFT (double precision, radix 2) -----------------------------------------

void fftInPlace(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * std::numbers::pi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        for (size_t i = 0; i < n; i += len) {
            for (size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> w = std::polar(1.0, ang * static_cast<double>(j));
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
            }
        }
    }
    if (inverse) {
        for (auto& x : a) x /= static_cast<double>(n);
    }
}

}  // namespace

// ---- Oscillator: PeriodicWave tables -----------------------------------------
//
// As Chromium builds them: a table of `size` samples per range, three ranges
// an octave, range r keeping 2^(-r/3) of the size/2 partials, every table
// scaled by the peak of the fullest one (unless normalization is off). A
// frequency reads the two ranges around it and crossfades them.

struct WaveTables {
    int size = 4096;
    int ranges = 36;
    double lowestFundamental = 0.0;
    std::vector<std::vector<float>> tables;  // ranges x size
};

namespace {

int waveSizeFor(double sampleRate) {
    if (sampleRate <= 24000.0) return 2048;
    if (sampleRate <= 88200.0) return 4096;
    return 16384;
}

std::shared_ptr<const WaveTables> buildTables(const std::vector<float>& real, const std::vector<float>& imag,
                                              bool normalize, double sampleRate) {
    auto t = std::make_shared<WaveTables>();
    t->size = waveSizeFor(sampleRate);
    t->ranges = static_cast<int>(std::lround(3.0 * std::log2(static_cast<double>(t->size))));
    t->lowestFundamental = sampleRate / t->size;
    const int maxPartials = t->size / 2;
    t->tables.resize(static_cast<size_t>(t->ranges));
    double scale = 1.0;
    for (int r = 0; r < t->ranges; ++r) {
        const int partials = static_cast<int>(std::pow(2.0, -r / 3.0) * maxPartials);
        std::vector<std::complex<double>> spec(static_cast<size_t>(t->size));
        const int count = std::min<int>(std::min(real.size(), imag.size()), maxPartials);
        // table[n] = sum_k real[k] cos(2 pi k n / N) + imag[k] sin(2 pi k n / N),
        // DC dropped, the partials past this range's count culled (the
        // count itself kept, as Chromium's tables have it).
        for (int k = 1; k < count && k <= partials; ++k)
            spec[static_cast<size_t>(k)] = std::complex<double>(real[static_cast<size_t>(k)], -imag[static_cast<size_t>(k)]);
        fftInPlace(spec, /*inverse=*/true);
        std::vector<float>& table = t->tables[static_cast<size_t>(r)];
        table.resize(static_cast<size_t>(t->size));
        for (int n = 0; n < t->size; ++n) table[static_cast<size_t>(n)] = static_cast<float>(spec[static_cast<size_t>(n)].real() * t->size);
        if (r == 0 && normalize) {
            float peak = 0.0f;
            for (float v : table) peak = std::max(peak, std::abs(v));
            scale = peak > 0.0f ? 1.0 / peak : 1.0;
        }
        if (normalize)
            for (float& v : table) v = static_cast<float>(v * scale);
    }
    return t;
}

// The built-in waves' coefficients (Web Audio, "Basic Waveform Phase").
std::shared_ptr<const WaveTables> builtinTables(const std::string& type, double sampleRate) {
    static std::mutex mu;
    static std::map<std::pair<std::string, int>, std::shared_ptr<const WaveTables>> cache;
    const std::lock_guard<std::mutex> lock(mu);
    const auto key = std::make_pair(type, static_cast<int>(sampleRate));
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const int n = waveSizeFor(sampleRate) / 2;
    std::vector<float> real(static_cast<size_t>(n), 0.0f), imag(static_cast<size_t>(n), 0.0f);
    const double pi = std::numbers::pi;
    for (int k = 1; k < n; ++k) {
        double b = 0.0;
        if (type == "sine") {
            b = k == 1 ? 1.0 : 0.0;
        } else if (type == "square") {
            b = (k & 1) ? 4.0 / (k * pi) : 0.0;
        } else if (type == "sawtooth") {
            b = ((k & 1) ? 2.0 : -2.0) / (k * pi);
        } else {  // triangle
            b = (k & 1) ? 8.0 * std::sin(k * pi / 2.0) / ((k * pi) * (k * pi)) : 0.0;
        }
        imag[static_cast<size_t>(k)] = static_cast<float>(b);
    }
    auto t = buildTables(real, imag, true, sampleRate);
    cache[key] = t;
    return t;
}

// The frame a scheduled time falls on: the first frame at or after it.
int64_t frameAt(double t, double sampleRate) {
    if (t <= 0.0) return 0;
    if (t >= 1e15) return INT64_MAX;
    return static_cast<int64_t>(std::ceil(t * sampleRate - 1e-9));
}

}  // namespace

void processOscillator(HostOscillatorNode& n, const float* freq, const float* detune, const KernelContext& k,
                       AudioBus& out) {
    out.setSilent(1);
    if (n.startTime < 0.0) return;
    const int64_t start = frameAt(n.startTime, k.sampleRate);
    const int64_t stop = frameAt(n.stopTime, k.sampleRate);
    if (start >= k.frame0 + kRenderQuantum || stop <= k.frame0) return;
    OfflineNodeState& s = stateOf(n.base);
    if (!s.tables) {
        s.tables = n.type == "custom" ? buildTables(n.waveReal, n.waveImag, n.waveNormalize, k.sampleRate)
                                      : builtinTables(n.type, k.sampleRate);
    }
    const WaveTables& t = *s.tables;
    const double size = t.size;
    float* o = out.channel(0);
    for (int i = 0; i < kRenderQuantum; ++i) {
        const int64_t f = k.frame0 + i;
        if (f < start || f >= stop) continue;
        const double fundamental = freq[i] * std::pow(2.0, detune[i] / 1200.0);
        if (f == start && !s.begun) {
            // Sub-sample accurate: the wave is where it would be had it begun
            // at the start time itself, between frames.
            s.begun = true;
            s.phase = (static_cast<double>(start) - n.startTime * k.sampleRate) * fundamental * size / k.sampleRate;
        }
        // The ranges around the frequency (Chromium's PeriodicWave::WaveDataForFundamentalFrequency).
        const double ratio = fundamental > 0.0 ? std::abs(fundamental) / t.lowestFundamental : 0.5;
        double range = 1.0 + std::log2(ratio) * 1200.0 / 400.0;
        range = std::clamp(range, 0.0, static_cast<double>(t.ranges - 1));
        const int r1 = static_cast<int>(range);
        const int r2 = r1 < t.ranges - 1 ? r1 + 1 : r1;
        const double mixf = range - r1;
        const std::vector<float>& higher = t.tables[static_cast<size_t>(r1)];
        const std::vector<float>& lower = t.tables[static_cast<size_t>(r2)];
        double idx = s.phase;
        idx -= std::floor(idx / size) * size;
        const int i0 = static_cast<int>(idx);
        const int i1 = (i0 + 1) % t.size;
        const double frac = idx - i0;
        const double hi = (1.0 - frac) * higher[static_cast<size_t>(i0)] + frac * higher[static_cast<size_t>(i1)];
        const double lo = (1.0 - frac) * lower[static_cast<size_t>(i0)] + frac * lower[static_cast<size_t>(i1)];
        o[i] = static_cast<float>((1.0 - mixf) * hi + mixf * lo);
        s.phase = idx + fundamental * size / k.sampleRate;
    }
}

void processConstantSource(HostConstantSourceNode& n, const float* offset, const KernelContext& k, AudioBus& out) {
    out.setSilent(1);
    if (n.startTime < 0.0) return;
    const int64_t start = frameAt(n.startTime, k.sampleRate);
    const int64_t stop = frameAt(n.stopTime, k.sampleRate);
    float* o = out.channel(0);
    for (int i = 0; i < kRenderQuantum; ++i) {
        const int64_t f = k.frame0 + i;
        if (f >= start && f < stop) o[i] = offset[i];
    }
}

void processBufferSource(HostAudioBufferSourceNode& n, const std::vector<std::vector<float>>& data,
                         double bufferRate, float playbackRate, float detune, const KernelContext& k,
                         AudioBus& out) {
    OfflineNodeState& s = stateOf(n.base);
    if (n.startTime < 0.0 || data.empty() || data[0].empty() || s.finished) {
        out.setSilent(1);
        return;
    }
    const int channels = static_cast<int>(data.size());
    out.setSilent(channels);
    const int64_t start = frameAt(n.startTime, k.sampleRate);
    const int64_t stop = frameAt(n.stopTime, k.sampleRate);
    const double len = static_cast<double>(data[0].size());
    // The playhead moves this many buffer frames an output frame.
    const double rate = static_cast<double>(playbackRate) * std::pow(2.0, detune / 1200.0) * bufferRate / k.sampleRate;
    double loopStart = 0.0, loopEnd = len;
    if (n.loop && (n.loopStart > 0.0 || n.loopEnd > 0.0) && n.loopStart >= 0.0 && n.loopEnd > 0.0 &&
        n.loopStart < n.loopEnd) {
        loopStart = std::min(n.loopStart * bufferRate, len);
        loopEnd = std::min(n.loopEnd * bufferRate, len);
    }
    const double maxPlayed = n.startDuration >= 0.0 ? n.startDuration * bufferRate : 1e300;
    for (int i = 0; i < kRenderQuantum; ++i) {
        const int64_t f = k.frame0 + i;
        if (f < start) continue;
        if (f >= stop || s.played >= maxPlayed) {
            s.finished = true;
            break;
        }
        if (!s.begun) {
            // Sub-sample accurate, as for the oscillator: the playhead is
            // where it would be had playback begun at the start time itself.
            s.begun = true;
            s.playhead = std::min(n.startOffset * bufferRate, len) +
                         std::max(0.0, static_cast<double>(start) - n.startTime * k.sampleRate) * rate;
            if (n.loop && s.playhead >= loopEnd) s.playhead = loopStart;
        }
        double pos = s.playhead;
        if (n.loop) {
            if (pos >= loopEnd) pos = loopStart + std::fmod(pos - loopStart, loopEnd - loopStart);
        } else if (pos >= len || pos < 0.0) {
            s.finished = true;
            break;
        }
        const int64_t i0 = static_cast<int64_t>(pos);
        const double frac = pos - static_cast<double>(i0);
        int64_t i1 = i0 + 1;
        if (n.loop && static_cast<double>(i1) >= loopEnd) i1 = static_cast<int64_t>(loopStart);
        for (int c = 0; c < channels; ++c) {
            const std::vector<float>& ch = data[static_cast<size_t>(c)];
            const double a = ch[static_cast<size_t>(i0)];
            const double b = i1 < static_cast<int64_t>(ch.size()) ? ch[static_cast<size_t>(i1)] : 0.0;
            out.channel(c)[i] = static_cast<float>(a + (b - a) * frac);
        }
        s.playhead = pos + rate;
        s.played += std::abs(rate);
    }
}

void processGain(const AudioBus& in, const float* gain, AudioBus& out) {
    out.setSilent(in.channels);
    for (int c = 0; c < in.channels; ++c) {
        const float* x = in.channel(c);
        float* y = out.channel(c);
        for (int i = 0; i < kRenderQuantum; ++i) y[i] = x[i] * gain[i];
    }
}

// ---- Biquad ---------------------------------------------------------------------

namespace {

struct Coefs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

// Web Audio's BiquadFilterNode formulas (the Audio EQ Cookbook).
Coefs biquadCoefs(const std::string& type, double f, double Q, double G, double sampleRate) {
    Coefs c;
    const double nyquist = sampleRate / 2.0;
    const double fn = std::clamp(f / nyquist, 0.0, 1.0);
    const double w0 = std::numbers::pi * fn;
    const double A = std::pow(10.0, G / 40.0);
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alphaQ = sw / (2.0 * Q);
    const double alphaQdB = sw / (2.0 * std::pow(10.0, Q / 20.0));
    const double alphaS = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / 1.0 - 1.0) + 2.0);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    if (type == "lowpass") {
        if (fn >= 1.0) return c;              // passes everything
        if (fn <= 0.0) { c.b0 = 0; return c; }  // passes nothing
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alphaQdB; a1 = -2 * cw; a2 = 1 - alphaQdB;
    } else if (type == "highpass") {
        if (fn >= 1.0) { c.b0 = 0; return c; }
        if (fn <= 0.0) return c;
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alphaQdB; a1 = -2 * cw; a2 = 1 - alphaQdB;
    } else if (type == "bandpass") {
        if (fn <= 0.0 || fn >= 1.0) { c.b0 = 0; return c; }
        if (Q <= 0.0) return c;
        b0 = alphaQ; b1 = 0; b2 = -alphaQ;
        a0 = 1 + alphaQ; a1 = -2 * cw; a2 = 1 - alphaQ;
    } else if (type == "notch") {
        if (fn <= 0.0 || fn >= 1.0) return c;
        if (Q <= 0.0) { c.b0 = 0; return c; }
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alphaQ; a1 = -2 * cw; a2 = 1 - alphaQ;
    } else if (type == "allpass") {
        if (fn <= 0.0 || fn >= 1.0) return c;
        if (Q <= 0.0) { c.b0 = -1; return c; }
        b0 = 1 - alphaQ; b1 = -2 * cw; b2 = 1 + alphaQ;
        a0 = 1 + alphaQ; a1 = -2 * cw; a2 = 1 - alphaQ;
    } else if (type == "peaking") {
        if (fn <= 0.0 || fn >= 1.0) return c;
        if (Q <= 0.0) { c.b0 = A * A; return c; }
        b0 = 1 + alphaQ * A; b1 = -2 * cw; b2 = 1 - alphaQ * A;
        a0 = 1 + alphaQ / A; a1 = -2 * cw; a2 = 1 - alphaQ / A;
    } else if (type == "lowshelf") {
        if (fn >= 1.0) { c.b0 = A * A; return c; }
        if (fn <= 0.0) return c;
        const double sa = 2 * alphaS * std::sqrt(A);
        b0 = A * ((A + 1) - (A - 1) * cw + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sa;
    } else if (type == "highshelf") {
        if (fn >= 1.0) return c;
        if (fn <= 0.0) { c.b0 = A * A; return c; }
        const double sa = 2 * alphaS * std::sqrt(A);
        b0 = A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sa;
    }
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
}

}  // namespace

void processBiquad(HostBiquadFilterNode& n, const AudioBus& in, const float* freq, const float* detune,
                   const float* q, const float* gain, bool perSample, const KernelContext& k, AudioBus& out) {
    out.setSilent(in.channels);
    OfflineNodeState& s = stateOf(n.base);
    if (s.mem.size() < static_cast<size_t>(in.channels) * 4) s.mem.resize(static_cast<size_t>(in.channels) * 4, 0.0);
    auto coefsAt = [&](int i) {
        return biquadCoefs(n.type, freq[i] * std::pow(2.0, detune[i] / 1200.0), q[i], gain[i], k.sampleRate);
    };
    Coefs c = coefsAt(0);
    for (int ch = 0; ch < in.channels; ++ch) {
        double* m = s.mem.data() + static_cast<size_t>(ch) * 4;
        const float* x = in.channel(ch);
        float* y = out.channel(ch);
        for (int i = 0; i < kRenderQuantum; ++i) {
            if (perSample && i > 0) c = coefsAt(i);
            const double v = c.b0 * x[i] + c.b1 * m[0] + c.b2 * m[1] - c.a1 * m[2] - c.a2 * m[3];
            m[1] = m[0];
            m[0] = x[i];
            m[3] = m[2];
            m[2] = std::abs(v) < 1e-30 ? 0.0 : v;  // no denormals
            y[i] = static_cast<float>(v);
        }
        if (perSample) c = coefsAt(0);
    }
}

void processIIR(HostIIRFilterNode& n, const AudioBus& in, AudioBus& out) {
    out.setSilent(in.channels);
    const size_t nf = n.feedforward.size(), nb = n.feedback.size();
    if (nb == 0 || n.feedback[0] == 0.0) return;
    const size_t order = std::max(nf, nb);
    OfflineNodeState& s = stateOf(n.base);
    // Per channel: the last `order` inputs, then the last `order` outputs.
    const size_t per = order * 2;
    if (s.mem.size() < per * static_cast<size_t>(in.channels)) s.mem.resize(per * static_cast<size_t>(in.channels), 0.0);
    const double a0 = n.feedback[0];
    for (int ch = 0; ch < in.channels; ++ch) {
        double* xm = s.mem.data() + per * static_cast<size_t>(ch);
        double* ym = xm + order;
        const float* x = in.channel(ch);
        float* y = out.channel(ch);
        for (int i = 0; i < kRenderQuantum; ++i) {
            double acc = n.feedforward.empty() ? 0.0 : n.feedforward[0] * x[i];
            for (size_t j = 1; j < nf; ++j) acc += n.feedforward[j] * xm[j - 1];
            for (size_t j = 1; j < nb; ++j) acc -= n.feedback[j] * ym[j - 1];
            acc /= a0;
            for (size_t j = order - 1; j > 0; --j) {
                xm[j] = xm[j - 1];
                ym[j] = ym[j - 1];
            }
            xm[0] = x[i];
            ym[0] = acc;
            y[i] = static_cast<float>(acc);
        }
    }
}

// ---- Delay ------------------------------------------------------------------------

namespace {

void ensureLine(HostDelayNode& n, OfflineNodeState& s, int channels, double sampleRate) {
    const size_t size = static_cast<size_t>(std::ceil(n.maxDelayTime * sampleRate)) + 2 * kRenderQuantum + 2;
    if (s.line.size() < static_cast<size_t>(channels)) s.line.resize(static_cast<size_t>(channels));
    for (auto& l : s.line) {
        if (l.size() != size) l.assign(size, 0.0f);
    }
}

}  // namespace

void delayWrite(HostDelayNode& n, const AudioBus& in, const KernelContext& k) {
    OfflineNodeState& s = stateOf(n.base);
    ensureLine(n, s, std::max<int>(in.channels, static_cast<int>(s.line.size())), k.sampleRate);
    const size_t size = s.line[0].size();
    for (size_t c = 0; c < s.line.size(); ++c) {
        for (int i = 0; i < kRenderQuantum; ++i) {
            const float v = static_cast<int>(c) < in.channels ? in.channel(static_cast<int>(c))[i] : 0.0f;
            s.line[c][static_cast<size_t>(s.written + i) % size] = v;
        }
    }
    s.written += kRenderQuantum;
}

void delayRead(HostDelayNode& n, const float* delayTime, bool inCycle, const KernelContext& k, AudioBus& out) {
    OfflineNodeState& s = stateOf(n.base);
    if (s.line.empty()) {
        out.setSilent(1);
        return;
    }
    const int channels = static_cast<int>(s.line.size());
    out.setSilent(channels);
    const size_t size = s.line[0].size();
    // The quantum's input is written: its first frame is written - 128.
    const int64_t base = s.written - kRenderQuantum;
    const double maxFrames = n.maxDelayTime * k.sampleRate;
    for (int i = 0; i < kRenderQuantum; ++i) {
        double d = std::clamp(static_cast<double>(delayTime[i]) * k.sampleRate, 0.0, maxFrames);
        if (inCycle) d = std::max(d, static_cast<double>(kRenderQuantum));
        const double pos = static_cast<double>(base + i) - d;
        if (pos < 0.0) continue;
        const int64_t p0 = static_cast<int64_t>(pos);
        const double frac = pos - static_cast<double>(p0);
        for (int c = 0; c < channels; ++c) {
            const std::vector<float>& l = s.line[static_cast<size_t>(c)];
            const double a = l[static_cast<size_t>(p0) % size];
            const double b = l[static_cast<size_t>(p0 + 1) % size];
            out.channel(c)[i] = static_cast<float>(a + (b - a) * frac);
        }
    }
}

// ---- Panners ------------------------------------------------------------------------

void processStereoPanner(const AudioBus& in, const float* pan, AudioBus& out) {
    out.setSilent(2);
    const double halfPi = std::numbers::pi / 2.0;
    float* l = out.channel(0);
    float* r = out.channel(1);
    if (in.channels == 1) {
        const float* m = in.channel(0);
        for (int i = 0; i < kRenderQuantum; ++i) {
            const double x = (std::clamp(static_cast<double>(pan[i]), -1.0, 1.0) + 1.0) / 2.0;
            l[i] = static_cast<float>(m[i] * std::cos(x * halfPi));
            r[i] = static_cast<float>(m[i] * std::sin(x * halfPi));
        }
        return;
    }
    const float* il = in.channel(0);
    const float* ir = in.channel(1);
    for (int i = 0; i < kRenderQuantum; ++i) {
        const double p = std::clamp(static_cast<double>(pan[i]), -1.0, 1.0);
        const double x = p <= 0.0 ? p + 1.0 : p;
        const double gl = std::cos(x * halfPi), gr = std::sin(x * halfPi);
        if (p <= 0.0) {
            l[i] = static_cast<float>(il[i] + ir[i] * gl);
            r[i] = static_cast<float>(ir[i] * gr);
        } else {
            l[i] = static_cast<float>(il[i] * gl);
            r[i] = static_cast<float>(ir[i] + il[i] * gr);
        }
    }
}

namespace {

void normalize3(double v[3]) {
    const double m = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (m > 0.0) {
        v[0] /= m;
        v[1] /= m;
        v[2] /= m;
    }
}

void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

void processPanner(HostPannerNode& n, const AudioBus& in, const ListenerState& listener, const KernelContext& k,
                   AudioBus& out) {
    out.setSilent(2);
    const double t = k.time0;
    double src[3], orient[3];
    for (int i = 0; i < 3; ++i) {
        src[i] = n.positionParams[i] ? n.positionParams[i]->evaluate(t) : 0.0;
        orient[i] = n.orientationParams[i] ? n.orientationParams[i]->evaluate(t) : (i == 0 ? 1.0 : 0.0);
    }
    // Azimuth (Web Audio, "Azimuth and Elevation").
    double rel[3] = {src[0] - listener.pos[0], src[1] - listener.pos[1], src[2] - listener.pos[2]};
    const double distance = std::sqrt(dot3(rel, rel));
    double azimuth = 0.0;
    if (distance > 0.0) {
        double dir[3] = {rel[0], rel[1], rel[2]};
        normalize3(dir);
        double front[3] = {listener.forward[0], listener.forward[1], listener.forward[2]};
        double up[3] = {listener.up[0], listener.up[1], listener.up[2]};
        double right[3];
        cross3(front, up, right);
        normalize3(right);
        double upn[3];
        cross3(right, front, upn);
        normalize3(upn);
        const double upProj = dot3(dir, upn);
        double proj[3] = {dir[0] - upProj * upn[0], dir[1] - upProj * upn[1], dir[2] - upProj * upn[2]};
        normalize3(proj);
        azimuth = std::acos(std::clamp(dot3(proj, right), -1.0, 1.0)) * 180.0 / std::numbers::pi;
        if (dot3(proj, front) < 0.0) azimuth = 360.0 - azimuth;
        azimuth = (azimuth >= 0.0 && azimuth <= 270.0) ? 90.0 - azimuth : 450.0 - azimuth;
    }
    if (azimuth < -90.0) azimuth = -180.0 - azimuth;
    else if (azimuth > 90.0) azimuth = 180.0 - azimuth;

    // Distance gain.
    double dg = 1.0;
    const double ref = n.refDistance, maxd = n.maxDistance, roll = n.rolloffFactor;
    if (n.distanceModel == "linear") {
        const double dd = std::clamp(distance, ref, maxd);
        dg = maxd > ref ? 1.0 - std::min(roll, 1.0) * (dd - ref) / (maxd - ref) : 1.0;
    } else if (n.distanceModel == "exponential") {
        dg = ref > 0.0 ? std::pow(std::max(distance, ref) / ref, -roll) : 1.0;
    } else {
        dg = ref > 0.0 ? ref / (ref + roll * (std::max(distance, ref) - ref)) : 1.0;
    }
    // Cone gain.
    double cg = 1.0;
    if (!(n.coneInnerAngle >= 360.0 && n.coneOuterAngle >= 360.0) && distance > 0.0) {
        double o[3] = {orient[0], orient[1], orient[2]};
        normalize3(o);
        if (dot3(o, o) > 0.0) {
            double toListener[3] = {-rel[0], -rel[1], -rel[2]};
            normalize3(toListener);
            const double angle = std::acos(std::clamp(dot3(toListener, o), -1.0, 1.0)) * 180.0 / std::numbers::pi;
            const double absAngle = std::abs(angle);
            const double inner = std::abs(n.coneInnerAngle) / 2.0, outer = std::abs(n.coneOuterAngle) / 2.0;
            if (absAngle <= inner) cg = 1.0;
            else if (absAngle >= outer) cg = n.coneOuterGain;
            else cg = 1.0 + (n.coneOuterGain - 1.0) * (absAngle - inner) / (outer - inner);
        }
    }
    const double g = dg * cg;
    const double halfPi = std::numbers::pi / 2.0;
    float* l = out.channel(0);
    float* r = out.channel(1);
    if (in.channels == 1) {
        const double x = (azimuth + 90.0) / 180.0;
        const double gl = std::cos(x * halfPi) * g, gr = std::sin(x * halfPi) * g;
        for (int i = 0; i < kRenderQuantum; ++i) {
            l[i] = static_cast<float>(in.channel(0)[i] * gl);
            r[i] = static_cast<float>(in.channel(0)[i] * gr);
        }
        return;
    }
    const double x = azimuth <= 0.0 ? (azimuth + 90.0) / 90.0 : azimuth / 90.0;
    const double gl = std::cos(x * halfPi), gr = std::sin(x * halfPi);
    for (int i = 0; i < kRenderQuantum; ++i) {
        const double il = in.channel(0)[i], ir = in.channel(1)[i];
        if (azimuth <= 0.0) {
            l[i] = static_cast<float>((il + ir * gl) * g);
            r[i] = static_cast<float>(ir * gr * g);
        } else {
            l[i] = static_cast<float>(il * gl * g);
            r[i] = static_cast<float>((ir + il * gr) * g);
        }
    }
}

// ---- WaveShaper -----------------------------------------------------------------------

void processWaveShaper(const HostWaveShaperNode& n, const AudioBus& in, AudioBus& out) {
    out.setSilent(in.channels);
    const size_t len = n.curve.size();
    for (int c = 0; c < in.channels; ++c) {
        const float* x = in.channel(c);
        float* y = out.channel(c);
        for (int i = 0; i < kRenderQuantum; ++i) {
            if (len == 0) {
                y[i] = x[i];
                continue;
            }
            const double v = (static_cast<double>(len) - 1.0) / 2.0 * (static_cast<double>(x[i]) + 1.0);
            if (v < 0.0) {
                y[i] = n.curve[0];
            } else if (v >= static_cast<double>(len) - 1.0) {
                y[i] = n.curve[len - 1];
            } else {
                const size_t k0 = static_cast<size_t>(v);
                const double f = v - static_cast<double>(k0);
                y[i] = static_cast<float>((1.0 - f) * n.curve[k0] + f * n.curve[k0 + 1]);
            }
        }
    }
}

// ---- Convolver -----------------------------------------------------------------------
//
// Uniformly partitioned (128-frame partitions, 256-point spectra), overlap-
// save. The impulse is scaled as Web Audio's "normalize" says (its RMS
// power, calibrated to -58 dB at 44.1 kHz).

void processConvolver(HostConvolverNode& n, const std::vector<std::vector<float>>& impulse, double impulseRate,
                      const AudioBus& in, AudioBus& out) {
    const int irCh = static_cast<int>(impulse.size());
    if (irCh == 0 || impulse[0].empty()) {
        out.setSilent(in.channels);
        return;
    }
    OfflineNodeState& s = stateOf(n.base);
    constexpr int B = kRenderQuantum, N = 2 * kRenderQuantum;
    if (s.irSpectra.empty()) {
        double scale = 1.0;
        if (n.normalize) {
            double power = 0.0;
            size_t count = 0;
            for (const auto& ch : impulse) {
                for (float v : ch) power += static_cast<double>(v) * v;
                count += ch.size();
            }
            power = std::sqrt(power / static_cast<double>(std::max<size_t>(count, 1)));
            power = std::max(power, 0.000125);
            scale = 0.00125 / power;
            if (impulseRate > 0.0) scale *= 44100.0 / impulseRate;
            if (irCh == 4) scale *= 0.5;
        }
        s.irSpectra.resize(static_cast<size_t>(irCh));
        for (int c = 0; c < irCh; ++c) {
            const auto& h = impulse[static_cast<size_t>(c)];
            const size_t parts = (h.size() + B - 1) / B;
            for (size_t p = 0; p < parts; ++p) {
                std::vector<std::complex<double>> sp(N);
                for (int i = 0; i < B && p * B + i < h.size(); ++i) sp[static_cast<size_t>(i)] = h[p * B + static_cast<size_t>(i)] * scale;
                fftInPlace(sp, false);
                s.irSpectra[static_cast<size_t>(c)].push_back(std::move(sp));
            }
        }
    }
    // Which input channel each output channel convolves with which impulse
    // channel(s) (Web Audio's ConvolverNode channel configurations).
    const int inCh = in.channels;
    const int outCh = (inCh == 1 && irCh == 1) ? 1 : 2;
    out.setSilent(outCh);
    const int useIn = std::min(inCh, 2);
    if (s.convChannelsIn != useIn) {
        s.convChannelsIn = useIn;
        s.inputSpectra.assign(static_cast<size_t>(useIn), {});
        s.lastInput.assign(static_cast<size_t>(useIn), std::vector<float>(B, 0.0f));
    }
    const size_t parts = s.irSpectra[0].size();
    for (int c = 0; c < useIn; ++c) {
        std::vector<std::complex<double>> sp(N);
        for (int i = 0; i < B; ++i) {
            sp[static_cast<size_t>(i)] = s.lastInput[static_cast<size_t>(c)][static_cast<size_t>(i)];
            sp[static_cast<size_t>(B + i)] = in.channel(c)[i];
        }
        std::copy(in.channel(c), in.channel(c) + B, s.lastInput[static_cast<size_t>(c)].begin());
        fftInPlace(sp, false);
        auto& hist = s.inputSpectra[static_cast<size_t>(c)];
        hist.insert(hist.begin(), std::move(sp));
        if (hist.size() > parts) hist.resize(parts);
    }
    auto convolve = [&](int inputCh, int irChannel, float* dst) {
        std::vector<std::complex<double>> acc(N);
        const auto& hist = s.inputSpectra[static_cast<size_t>(inputCh)];
        const auto& ir = s.irSpectra[static_cast<size_t>(irChannel)];
        for (size_t p = 0; p < hist.size() && p < ir.size(); ++p)
            for (int i = 0; i < N; ++i) acc[static_cast<size_t>(i)] += hist[p][static_cast<size_t>(i)] * ir[p][static_cast<size_t>(i)];
        fftInPlace(acc, true);
        for (int i = 0; i < B; ++i) dst[i] += static_cast<float>(acc[static_cast<size_t>(B + i)].real());
    };
    if (irCh >= 4 && useIn == 2) {
        convolve(0, 0, out.channel(0));
        convolve(1, 2, out.channel(0));
        convolve(0, 1, out.channel(1));
        convolve(1, 3, out.channel(1));
    } else if (outCh == 1) {
        convolve(0, 0, out.channel(0));
    } else {
        const int r0 = 0, r1 = irCh >= 2 ? 1 : 0;
        convolve(0, r0, out.channel(0));
        convolve(useIn == 2 ? 1 : 0, r1, out.channel(1));
    }
}

// ---- DynamicsCompressor -------------------------------------------------------------
//
// A soft-knee compressor on the loudest channel with Web Audio's 6 ms
// lookahead and its makeup gain ((1 / curve(1))^0.6). Its parameters are
// k-rate. Chromium's kernel differs in its adaptive release, so its output
// is close to, not the same as, Chromium's.

void processCompressor(HostDynamicsCompressorNode& n, const AudioBus& in, const KernelContext& k, AudioBus& out) {
    const double t = k.time0;
    const double threshold = n.thresholdParam ? n.thresholdParam->evaluate(t) : -24.0;
    const double knee = n.kneeParam ? n.kneeParam->evaluate(t) : 30.0;
    const double ratio = std::max(1.0, n.ratioParam ? static_cast<double>(n.ratioParam->evaluate(t)) : 12.0);
    const double attack = std::max(0.0001, n.attackParam ? static_cast<double>(n.attackParam->evaluate(t)) : 0.003);
    const double release = std::max(0.0001, n.releaseParam ? static_cast<double>(n.releaseParam->evaluate(t)) : 0.25);
    out.setSilent(in.channels);
    OfflineNodeState& s = stateOf(n.base);
    const int delay = static_cast<int>(0.006 * k.sampleRate);
    if (s.lookahead.size() != static_cast<size_t>(in.channels)) {
        s.lookahead.assign(static_cast<size_t>(in.channels), std::vector<float>(static_cast<size_t>(delay + 1), 0.0f));
        s.lookaheadPos = 0;
    }
    auto curveDb = [&](double xDb) {
        // Gain computer with a knee of `knee` dB centred on the threshold.
        if (knee > 0.0 && 2.0 * std::abs(xDb - threshold) <= knee) {
            const double d = xDb - threshold + knee / 2.0;
            return xDb + (1.0 / ratio - 1.0) * d * d / (2.0 * knee);
        }
        if (xDb > threshold) return threshold + (xDb - threshold) / ratio;
        return xDb;
    };
    const double makeup = std::pow(1.0 / std::pow(10.0, curveDb(0.0) / 20.0), 0.6);
    const double att = std::exp(-1.0 / (attack * k.sampleRate));
    const double rel = std::exp(-1.0 / (release * k.sampleRate));
    for (int i = 0; i < kRenderQuantum; ++i) {
        double peak = 0.0;
        for (int c = 0; c < in.channels; ++c) peak = std::max(peak, static_cast<double>(std::abs(in.channel(c)[i])));
        const double xDb = peak > 1e-9 ? 20.0 * std::log10(peak) : -180.0;
        const double target = curveDb(xDb) - xDb;  // <= 0
        const double coef = target < s.gainDb ? att : rel;
        s.gainDb = target + coef * (s.gainDb - target);
        const double g = std::pow(10.0, s.gainDb / 20.0) * makeup;
        for (int c = 0; c < in.channels; ++c) {
            auto& line = s.lookahead[static_cast<size_t>(c)];
            const float delayed = line[static_cast<size_t>(s.lookaheadPos)];
            line[static_cast<size_t>(s.lookaheadPos)] = in.channel(c)[i];
            out.channel(c)[i] = static_cast<float>(delayed * g);
        }
        s.lookaheadPos = (s.lookaheadPos + 1) % static_cast<int>(s.lookahead[0].size());
    }
    n.reduction = static_cast<float>(s.gainDb);
}

// ---- Params ----------------------------------------------------------------------------

void computeParam(const HostAudioParam& p, const AudioBus* input, double t0, double sampleRate, float* out) {
    const bool constant = p.kRate || p.timeline.empty();
    if (constant) {
        const float v = p.evaluate(t0);
        std::fill(out, out + kRenderQuantum, v);
    } else {
        for (int i = 0; i < kRenderQuantum; ++i) out[i] = p.evaluate(t0 + i / sampleRate);
    }
    if (!input) return;
    // Connected audio, down-mixed to mono, adds to the intrinsic value.
    AudioBus mono;
    mono.setSilent(1);
    mixInto(*input, mono, false);
    const float* m = mono.channel(0);
    for (int i = 0; i < kRenderQuantum; ++i) {
        const float add = p.kRate ? m[0] : m[i];
        out[i] = std::clamp(out[i] + add, p.minValue, p.maxValue);
    }
}

}  // namespace broaudio::api
