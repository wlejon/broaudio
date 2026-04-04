#include "broaudio/dsp/limiter.h"
#include <cmath>
#include <algorithm>

namespace broaudio {

Limiter::Limiter(int sampleRate, int channels)
    : sampleRate_(sampleRate), channels_(channels)
{
    updateCoefficients();
    updateDelayBuffer();
}

void Limiter::reset()
{
    envelope_ = 0.0f;
    std::fill(delayBuffer_.begin(), delayBuffer_.end(), 0.0f);
    delayWritePos_ = 0;
    delayReadPos_ = 0;
}

void Limiter::updateCoefficients()
{
    attackCoeff_ = std::exp(-1.0f / (attack_ * 0.001f * sampleRate_));
    releaseCoeff_ = std::exp(-1.0f / (release_ * 0.001f * sampleRate_));
}

void Limiter::updateDelayBuffer()
{
    delayLength_ = static_cast<size_t>(std::max(lookAhead_, 0.1f) * 0.001f * sampleRate_ * channels_);
    delayBuffer_.resize(delayLength_ + channels_, 0.0f);
    delayReadPos_ = (delayWritePos_ + channels_) % delayLength_;
}

void Limiter::process(float* buffer, size_t frames)
{
    if (!enabled_) return;
    updateCoefficients();

    for (size_t i = 0; i < frames * channels_; ++i) {
        // Compute envelope from the incoming (non-delayed) sample
        float incomingDb = 20.0f * std::log10(std::max(std::abs(buffer[i]), 1e-6f));
        float gainReduction = 0.0f;
        if (incomingDb > threshold_) {
            float excess = incomingDb - threshold_;
            gainReduction = excess * (1.0f - 1.0f / ratio_);
        }
        if (gainReduction > envelope_)
            envelope_ = gainReduction + (envelope_ - gainReduction) * attackCoeff_;
        else
            envelope_ = gainReduction + (envelope_ - gainReduction) * releaseCoeff_;

        float gain = std::pow(10.0f, -envelope_ / 20.0f);

        // Write incoming sample into delay line, read delayed sample out
        delayBuffer_[delayWritePos_] = buffer[i];
        delayWritePos_ = (delayWritePos_ + 1) % delayLength_;

        buffer[i] = delayBuffer_[delayReadPos_] * gain;
        delayReadPos_ = (delayReadPos_ + 1) % delayLength_;
    }
}

} // namespace broaudio
