#pragma once
// Helpers shared by the physical-distance tests (air absorption, propagation
// delay, ramps, convolution): render a headless engine into a recording, and
// measure what came out.

#include "broaudio/engine.h"
#include "broaudio/dsp/fft.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace dtest {

constexpr float kPi = std::numbers::pi_v<float>;

// Deterministic white noise in [-amp, amp].
inline std::vector<float> noise(int frames, float amp, uint32_t seed = 12345u) {
    std::vector<float> v(frames);
    uint32_t s = seed;
    for (int i = 0; i < frames; i++) {
        s = s * 1664525u + 1013904223u;
        v[i] = amp * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
    }
    return v;
}

inline std::vector<float> sine(int frames, float hz, float amp, int sr) {
    std::vector<float> v(frames);
    for (int i = 0; i < frames; i++) v[i] = amp * std::sin(2.0f * kPi * hz * i / sr);
    return v;
}

// Render `frames` in `chunk`-sized blocks, calling `perChunk(frameIndex)` before
// each, and return the stereo interleaved recording (after master gain and
// limiter, so keep levels low).
template <class F>
std::vector<float> record(broaudio::Engine& e, int frames, int chunk, F perChunk) {
    e.startRecording(2, frames / static_cast<double>(e.sampleRate()) + 1.0);
    for (int done = 0; done < frames; done += chunk) {
        perChunk(done);
        e.renderBlock(std::min(chunk, frames - done));
    }
    e.stopRecording();
    return e.getRecordBuffer();
}

inline std::vector<float> record(broaudio::Engine& e, int frames, int chunk = 512) {
    return record(e, frames, chunk, [](int) {});
}

inline std::vector<float> left(const std::vector<float>& st) {
    std::vector<float> l(st.size() / 2);
    for (size_t i = 0; i < l.size(); i++) l[i] = st[2 * i];
    return l;
}

// Spectral centroid (Hz) of x[from, from+len) from averaged Hann-windowed
// 4096-point power spectra.
inline double centroid(const std::vector<float>& x, int from, int len, int sr) {
    constexpr int N = 4096;
    std::vector<double> power(N / 2, 0.0);
    std::vector<float> re(N), im(N);
    for (int off = from; off + N <= from + len; off += N / 2) {
        for (int i = 0; i < N; i++) {
            float w = 0.5f - 0.5f * std::cos(2.0f * kPi * i / (N - 1));
            re[i] = x[off + i] * w;
            im[i] = 0.0f;
        }
        broaudio::fft(re.data(), im.data(), N);
        for (int k = 0; k < N / 2; k++) power[k] += re[k] * re[k] + im[k] * im[k];
    }
    double num = 0.0, den = 0.0;
    for (int k = 1; k < N / 2; k++) {
        double f = static_cast<double>(k) * sr / N;
        num += f * power[k];
        den += power[k];
    }
    return den > 0.0 ? num / den : 0.0;
}

inline double rms(const std::vector<float>& x, int from, int len) {
    double s = 0.0;
    for (int i = from; i < from + len; i++) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / len);
}

inline int firstAbove(const std::vector<float>& x, float thr) {
    for (size_t i = 0; i < x.size(); i++)
        if (std::fabs(x[i]) > thr) return static_cast<int>(i);
    return -1;
}

} // namespace dtest
