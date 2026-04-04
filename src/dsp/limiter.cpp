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
    delayLength_ = static_cast<size_t>(lookAhead_ * 0.001f * sampleRate_ * channels_);
    delayBuffer_.resize(delayLength_ + channels_, 0.0f);
    delayReadPos_ = (delayWritePos_ + channels_) % delayLength_;
}

float Limiter::processSample(float sample)
{
    if (!enabled_) return sample;

    float sampleDb = 20.0f * std::log10(std::max(std::abs(sample), 1e-6f));

    float gainReduction = 0.0f;
    if (sampleDb > threshold_) {
        float excess = sampleDb - threshold_;
        gainReduction = excess * (1.0f - 1.0f / ratio_);
    }

    float targetEnvelope = gainReduction;
    if (targetEnvelope > envelope_)
        envelope_ = targetEnvelope + (envelope_ - targetEnvelope) * attackCoeff_;
    else
        envelope_ = targetEnvelope + (envelope_ - targetEnvelope) * releaseCoeff_;

    float gain = std::pow(10.0f, -envelope_ / 20.0f);
    return sample * gain;
}

void Limiter::process(float* buffer, size_t frames)
{
    if (!enabled_) return;
    updateCoefficients();

    for (size_t i = 0; i < frames * channels_; ++i) {
        delayBuffer_[delayWritePos_] = buffer[i];
        delayWritePos_ = (delayWritePos_ + 1) % delayLength_;

        float lookaheadSample = delayBuffer_[delayReadPos_];
        buffer[i] = processSample(lookaheadSample);
        delayReadPos_ = (delayReadPos_ + 1) % delayLength_;
    }
}

} // namespace broaudio
