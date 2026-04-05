#include "broaudio/dsp/limiter.h"
#include <cmath>
#include <algorithm>

namespace broaudio {

Limiter::Limiter(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels)
{
    rebuildLookahead();
}

void Limiter::setLookAhead(float lookAheadMs)
{
    lookAheadMs_ = std::max(0.1f, lookAheadMs);
    rebuildLookahead();
}

void Limiter::rebuildLookahead()
{
    size_t frames = static_cast<size_t>(
        std::max(1.0f, lookAheadMs_ * 0.001f * static_cast<float>(sampleRate_)));

    delayLen_ = frames * channels_;
    delayBuf_.assign(delayLen_, 0.0f);
    delayPos_ = 0;

    peakLen_ = frames;
    peakBuf_.assign(peakLen_, 0.0f);
    peakPos_ = 0;

    releaseCoeff_ = std::exp(-1.0f / (releaseMs_ * 0.001f * static_cast<float>(sampleRate_)));
    envelope_ = 1.0f;
}

void Limiter::reset()
{
    std::fill(delayBuf_.begin(), delayBuf_.end(), 0.0f);
    std::fill(peakBuf_.begin(), peakBuf_.end(), 0.0f);
    delayPos_ = 0;
    peakPos_ = 0;
    envelope_ = 1.0f;
}

void Limiter::process(float* buffer, size_t frames)
{
    if (!enabled_) return;

    float threshold = thresholdLin_;
    float relCoeff = std::exp(-1.0f / (releaseMs_ * 0.001f * static_cast<float>(sampleRate_)));

    for (size_t f = 0; f < frames; f++) {
        // Find the peak of the current input frame (across channels)
        float framePeak = 0.0f;
        for (int ch = 0; ch < channels_; ch++) {
            float a = std::fabs(buffer[f * channels_ + ch]);
            if (a > framePeak) framePeak = a;
        }

        // Write peak into the circular lookahead peak buffer
        peakBuf_[peakPos_] = framePeak;
        peakPos_ = (peakPos_ + 1) % peakLen_;

        // Find the maximum peak across the entire lookahead window.
        // This lets us know the worst-case level that's coming and
        // pre-apply enough gain reduction before it arrives.
        float maxPeak = 0.0f;
        for (size_t i = 0; i < peakLen_; i++) {
            if (peakBuf_[i] > maxPeak) maxPeak = peakBuf_[i];
        }

        // Compute the gain needed to keep maxPeak at or below threshold
        float targetGain = 1.0f;
        if (maxPeak > threshold) {
            targetGain = threshold / maxPeak;
        }

        // Envelope: instant attack (grab new minimum immediately),
        // smooth release (let gain recover gradually)
        if (targetGain < envelope_) {
            envelope_ = targetGain;  // instant attack
        } else {
            envelope_ = targetGain + (envelope_ - targetGain) * relCoeff;
        }

        // Write incoming samples into delay line, read delayed samples out
        for (int ch = 0; ch < channels_; ch++) {
            size_t idx = f * channels_ + ch;
            size_t dIdx = delayPos_ + ch;

            float delayed = delayBuf_[dIdx];
            delayBuf_[dIdx] = buffer[idx];
            buffer[idx] = delayed * envelope_;
        }
        delayPos_ = (delayPos_ + channels_) % delayLen_;
    }
}

} // namespace broaudio
