// Seamless loops cut from a synthesis voice (SynthLoopOptions in
// synth/synth_graph.h).
//
// The voice runs without an end for start + length + crossfade frames. The
// loop is its frames [start, start + length); over the first `crossfade` of
// them the continuation [start + length, ...) fades out as the loop's own
// start fades in, layer by layer (each layer with the curve its own halves'
// correlation calls for), the blended layers then summed exactly as the
// voice sums them. Loop sample 0 is therefore exactly voice sample
// start + length: the seam (last loop sample, then the first) is two
// consecutive samples of the voice, and every filter, resonator and comb
// state is continuous across it by construction. A copy of the voice taken
// at start + length, with its envelopes released, renders the release tail a
// looping voice crossfades into on note-off.
//
// All of it is double-precision arithmetic on the rendered floats, computed
// once; the live voice plays these samples, so it equals the offline render.

#include "synth_voice_data.h"

#include <algorithm>
#include <cmath>

namespace broaudio {

namespace {

constexpr double kHalfPi = 1.57079632679489661923;
constexpr double kMaxLoopSeconds = 60.0;
constexpr double kMaxStartSeconds = 600.0;
constexpr double kMaxTailSeconds = 10.0;
constexpr int kBlock = 4096;

int64_t framesOf(double seconds, int fs, double maxSeconds)
{
    if (!(seconds > 0.0)) return 0;
    return static_cast<int64_t>(std::llround(std::min(seconds, maxSeconds) * fs));
}

// Render n frames of `v` into dst (or discard them when dst is null).
void run(SynthVoiceData& v, int64_t n, float* dst, bool compiled)
{
    float tmp[kBlock];
    while (n > 0) {
        const int m = static_cast<int>(std::min<int64_t>(n, kBlock));
        float* o = dst ? dst : tmp;
        v.render(o, m, compiled);
        if (dst) dst += m;
        n -= m;
    }
}

// Correlation of the two crossfaded halves, clamped to [0, 1].
double correlation(const float* a, const float* b, int64_t n)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    if (!(aa > 0.0) || !(bb > 0.0)) return 0.0;
    return std::clamp(ab / std::sqrt(aa * bb), 0.0, 1.0);
}

} // namespace

int64_t synthLoopFrames(const SynthLoopOptions& o, int fs)
{
    return std::max<int64_t>(2, framesOf(o.length, fs, kMaxLoopSeconds));
}

std::unique_ptr<SynthLoopData> buildSynthLoop(SynthVoiceData& v, const SynthGraphData& g,
                                              const SynthLoopOptions& o, bool compiled)
{
    auto L = std::make_unique<SynthLoopData>();
    const int fs = v.fs;
    const int64_t len = synthLoopFrames(o, fs);
    const int64_t fade = std::min(framesOf(o.crossfade, fs, kMaxLoopSeconds), len / 2);
    const int64_t start = framesOf(o.start, fs, kMaxStartSeconds);

    v.endless = true;
    v.duration = -1;
    run(v, start, nullptr, compiled);
    L->loop.assign(static_cast<size_t>(len), 0.0f);
    // The crossfaded stretches are rendered layer by layer (own: the loop's
    // first `fade` frames; cont: the frames after its end) so each layer
    // fades with the curve its own halves call for; the rest is the mix.
    const int layers = static_cast<int>(v.layers.size());
    std::vector<std::vector<float>> own(layers, std::vector<float>(static_cast<size_t>(fade)));
    std::vector<std::vector<float>> cont = own;
    auto runLayers = [&](std::vector<std::vector<float>>& dst) {
        std::vector<float*> ptr(layers);
        for (int64_t done = 0; done < fade;) {
            const int m = static_cast<int>(std::min<int64_t>(fade - done, kBlock));
            for (int l = 0; l < layers; ++l) ptr[l] = dst[l].data() + done;
            v.renderLayers(ptr.data(), m, compiled);
            done += m;
        }
    };
    runLayers(own);
    run(v, len - fade, L->loop.data() + fade, compiled);
    // The voice at the seam: the release tail starts here.
    std::unique_ptr<SynthVoiceData> tail = v.clone();
    runLayers(cont);

    // The crossfade, per layer: blend[i] = own[i] * in(t) + cont[i] * out(t),
    // t = i / fade, then the layers summed as the voice sums them, so
    // loop[0] is the continuation's first sample exactly. 'auto' corrects
    // each layer's equal-power gains for that layer's correlation.
    if (fade > 0) {
        std::vector<std::vector<float>> blend = own;
        for (int l = 0; l < layers; ++l) {
            const double r = o.curve == SynthLoopCurve::Auto ? correlation(own[l].data(), cont[l].data(), fade) : 0.0;
            for (int64_t i = 0; i < fade; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(fade);
                double gin, gout;
                if (o.curve == SynthLoopCurve::Linear) {
                    gin = t;
                    gout = 1.0 - t;
                } else {
                    gin = std::sin(kHalfPi * t);
                    gout = std::cos(kHalfPi * t);
                    const double norm = std::sqrt(1.0 + 2.0 * r * gin * gout);
                    gin /= norm;
                    gout /= norm;
                }
                blend[l][i] = static_cast<float>(static_cast<double>(own[l][i]) * gin +
                                                 static_cast<double>(cont[l][i]) * gout);
            }
        }
        std::vector<const float*> ptr(layers);
        for (int l = 0; l < layers; ++l) ptr[l] = blend[l].data();
        mixSynthLayers(ptr.data(), layers, L->loop.data(), static_cast<int>(fade));
    }

    // The release: the voice from the seam with its envelopes released,
    // until it ends (or the cap, faded). Without an amplitude envelope there
    // is nothing to release: the loop fades out.
    const int64_t relFade = framesOf(o.releaseFade, fs, kMaxLoopSeconds);
    if (g.ampEnvs > 0) {
        tail->endless = false;
        tail->releaseAll();
        const int64_t cap = framesOf(kMaxTailSeconds, fs, kMaxTailSeconds);
        float tmp[kBlock];
        while (!tail->done && static_cast<int64_t>(L->tail.size()) < cap) {
            const int m = static_cast<int>(std::min<int64_t>(kBlock, cap - static_cast<int64_t>(L->tail.size())));
            const int64_t at = tail->pos;
            tail->render(tmp, m, compiled);
            const int64_t keep = tail->done ? std::clamp<int64_t>(tail->end - at, 0, m) : m;
            L->tail.insert(L->tail.end(), tmp, tmp + keep);
        }
        if (!tail->done) {
            const int64_t n = static_cast<int64_t>(L->tail.size());
            const int64_t f = std::min(n, std::max<int64_t>(relFade, fs / 20));
            for (int64_t i = 0; i < f; ++i)
                L->tail[n - f + i] *= static_cast<float>(static_cast<double>(f - i) / static_cast<double>(f + 1));
        }
    }
    L->fadeIn.resize(static_cast<size_t>(relFade));
    L->fadeOut.resize(static_cast<size_t>(relFade));
    for (int64_t k = 0; k < relFade; ++k) {
        const double t = (static_cast<double>(k) + 0.5) / static_cast<double>(relFade);
        L->fadeIn[k] = static_cast<float>(std::sin(kHalfPi * t));
        L->fadeOut[k] = static_cast<float>(std::cos(kHalfPi * t));
    }
    return L;
}

} // namespace broaudio
