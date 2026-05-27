#include "broaudio/mic_tap.h"

#include <algorithm>
#include <cmath>

namespace broaudio {

void Agc::apply(float* samples, int n, int sampleRate)
{
    if (n <= 0 || sampleRate <= 0) return;

    float chunkPeak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float a = std::fabs(samples[i]);
        if (a > chunkPeak) chunkPeak = a;
    }

    const float decay = std::pow(
        0.5f,
        static_cast<float>(n) /
            (cfg_.halfLifeSec * static_cast<float>(sampleRate)));

    if (chunkPeak > cfg_.noiseGate) {
        runningPeak_ = std::max(runningPeak_ * decay, chunkPeak);
    } else {
        runningPeak_ *= decay;
    }

    if (runningPeak_ <= 0.0f) return;
    const float gain = std::min(cfg_.maxGain, cfg_.targetPeak / runningPeak_);
    if (gain <= 1.0f) return;
    for (int i = 0; i < n; ++i) {
        samples[i] *= gain;
    }
}

} // namespace broaudio
