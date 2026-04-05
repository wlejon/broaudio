#include "broaudio/dsp/equalizer.h"
#include <cmath>
#include <algorithm>
#include <numbers>

namespace broaudio {

Equalizer::Equalizer(int sampleRate) : sampleRate_(sampleRate)
{
    for (int i = 0; i < NUM_BANDS; ++i) {
        bands_[i].frequency = BAND_FREQUENCIES[i];
        rightBands_[i] = bands_[i];
        updateFilterCoefficients(i);
    }
}

void Equalizer::setSampleRate(int sampleRate)
{
    sampleRate_ = sampleRate;
    for (int i = 0; i < NUM_BANDS; ++i)
        updateFilterCoefficients(i);
    reset();
}

void Equalizer::setMasterGain(float gain)
{
    masterGain_ = std::max(0.0f, std::min(11.0f, gain));
}

void Equalizer::setBandGain(int bandIndex, float gain)
{
    if (bandIndex < 0 || bandIndex >= NUM_BANDS) return;
    bands_[bandIndex].gain = std::max(-12.0f, std::min(12.0f, gain));
    rightBands_[bandIndex].gain = bands_[bandIndex].gain;
    rightBands_[bandIndex].frequency = bands_[bandIndex].frequency;
    rightBands_[bandIndex].q = bands_[bandIndex].q;
    updateFilterCoefficients(bandIndex);
}

void Equalizer::setEnabled(bool enabled)
{
    enabled_ = enabled;
    if (!enabled) reset();
}

void Equalizer::reset()
{
    for (auto& b : bands_) b.x1 = b.x2 = b.y1 = b.y2 = 0.0f;
    for (auto& b : rightBands_) b.x1 = b.x2 = b.y1 = b.y2 = 0.0f;
}

float Equalizer::getBandGain(int bandIndex) const
{
    if (bandIndex >= 0 && bandIndex < NUM_BANDS) return bands_[bandIndex].gain;
    return 0.0f;
}

void Equalizer::process(float* buffer, int numSamples)
{
    if (!enabled_) return;
    float masterLinearGain = std::pow(10.0f, masterGain_ / 20.0f);

    for (int i = 0; i < numSamples; ++i) {
        float output = processSample(buffer[i]) * masterLinearGain;
        if (output > 1.0f)
            output = 1.0f - std::exp(-output + 1.0f);
        else if (output < -1.0f)
            output = -1.0f + std::exp(output + 1.0f);
        buffer[i] = output;
    }
}

void Equalizer::processStereo(float* leftBuffer, float* rightBuffer, int numSamples)
{
    if (!enabled_) return;
    float masterLinearGain = std::pow(10.0f, masterGain_ / 20.0f);

    for (int i = 0; i < numSamples; ++i) {
        float leftOutput = leftBuffer[i];
        for (auto& band : bands_) {
            if (band.gain != 0.0f) {
                float x0 = leftOutput;
                float y0 = band.b0 * x0 + band.b1 * band.x1 + band.b2 * band.x2
                          - band.a1 * band.y1 - band.a2 * band.y2;
                y0 += 1e-30f;
                band.x2 = band.x1; band.x1 = x0;
                band.y2 = band.y1; band.y1 = y0;
                leftOutput = y0;
            }
        }
        leftOutput *= masterLinearGain;
        if (leftOutput > 1.0f) leftOutput = 1.0f - std::exp(-leftOutput + 1.0f);
        else if (leftOutput < -1.0f) leftOutput = -1.0f + std::exp(leftOutput + 1.0f);
        leftBuffer[i] = leftOutput;

        float rightOutput = rightBuffer[i];
        for (auto& band : rightBands_) {
            if (band.gain != 0.0f) {
                float x0 = rightOutput;
                float y0 = band.b0 * x0 + band.b1 * band.x1 + band.b2 * band.x2
                          - band.a1 * band.y1 - band.a2 * band.y2;
                y0 += 1e-30f;
                band.x2 = band.x1; band.x1 = x0;
                band.y2 = band.y1; band.y1 = y0;
                rightOutput = y0;
            }
        }
        rightOutput *= masterLinearGain;
        if (rightOutput > 1.0f) rightOutput = 1.0f - std::exp(-rightOutput + 1.0f);
        else if (rightOutput < -1.0f) rightOutput = -1.0f + std::exp(rightOutput + 1.0f);
        rightBuffer[i] = rightOutput;
    }
}

void Equalizer::processStereoInterleaved(float* buffer, int numFrames)
{
    if (!enabled_) return;
    float masterLinearGain = std::pow(10.0f, masterGain_ / 20.0f);

    for (int i = 0; i < numFrames; ++i) {
        float leftOutput = buffer[i * 2];
        for (auto& band : bands_) {
            if (band.gain != 0.0f) {
                float x0 = leftOutput;
                float y0 = band.b0 * x0 + band.b1 * band.x1 + band.b2 * band.x2
                          - band.a1 * band.y1 - band.a2 * band.y2;
                y0 += 1e-30f;
                band.x2 = band.x1; band.x1 = x0;
                band.y2 = band.y1; band.y1 = y0;
                leftOutput = y0;
            }
        }
        leftOutput *= masterLinearGain;
        if (leftOutput > 1.0f) leftOutput = 1.0f - std::exp(-leftOutput + 1.0f);
        else if (leftOutput < -1.0f) leftOutput = -1.0f + std::exp(leftOutput + 1.0f);
        buffer[i * 2] = leftOutput;

        float rightOutput = buffer[i * 2 + 1];
        for (auto& band : rightBands_) {
            if (band.gain != 0.0f) {
                float x0 = rightOutput;
                float y0 = band.b0 * x0 + band.b1 * band.x1 + band.b2 * band.x2
                          - band.a1 * band.y1 - band.a2 * band.y2;
                y0 += 1e-30f;
                band.x2 = band.x1; band.x1 = x0;
                band.y2 = band.y1; band.y1 = y0;
                rightOutput = y0;
            }
        }
        rightOutput *= masterLinearGain;
        if (rightOutput > 1.0f) rightOutput = 1.0f - std::exp(-rightOutput + 1.0f);
        else if (rightOutput < -1.0f) rightOutput = -1.0f + std::exp(rightOutput + 1.0f);
        buffer[i * 2 + 1] = rightOutput;
    }
}

float Equalizer::processSample(float input)
{
    float output = input;
    for (auto& band : bands_) {
        if (band.gain != 0.0f) {
            float x0 = output;
            float y0 = band.b0 * x0 + band.b1 * band.x1 + band.b2 * band.x2
                      - band.a1 * band.y1 - band.a2 * band.y2;
            y0 += 1e-30f;
            band.x2 = band.x1; band.x1 = x0;
            band.y2 = band.y1; band.y1 = y0;
            output = y0;
        }
    }
    return output;
}

void Equalizer::updateFilterCoefficients(int bandIndex)
{
    if (bandIndex < 0 || bandIndex >= NUM_BANDS) return;
    auto& band = bands_[bandIndex];
    auto& right = rightBands_[bandIndex];

    if (band.gain == 0.0f) {
        band.a0 = right.a0 = 1.0f;
        band.a1 = right.a1 = 0.0f;
        band.a2 = right.a2 = 0.0f;
        band.b0 = right.b0 = 1.0f;
        band.b1 = right.b1 = 0.0f;
        band.b2 = right.b2 = 0.0f;
    } else {
        calculatePeakingEQ(band.frequency, band.gain, band.q,
                          band.a0, band.a1, band.a2, band.b0, band.b1, band.b2);
        right.a0 = band.a0; right.a1 = band.a1; right.a2 = band.a2;
        right.b0 = band.b0; right.b1 = band.b1; right.b2 = band.b2;
    }
}

void Equalizer::calculatePeakingEQ(float frequency, float gain, float q,
                                    float& a0, float& a1, float& a2,
                                    float& b0, float& b1, float& b2)
{
    float A = std::pow(10.0f, gain / 40.0f);
    float omega = 2.0f * std::numbers::pi_v<float> * frequency / static_cast<float>(sampleRate_);
    float sin_omega = std::sin(omega);
    float cos_omega = std::cos(omega);
    float alpha = sin_omega / (2.0f * q);

    float a0_temp = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0_temp;
    b1 = (-2.0f * cos_omega) / a0_temp;
    b2 = (1.0f - alpha * A) / a0_temp;
    a0 = 1.0f;
    a1 = (-2.0f * cos_omega) / a0_temp;
    a2 = (1.0f - alpha / A) / a0_temp;
}

void Equalizer::applyPreset(Preset preset)
{
    switch (preset) {
        case Preset::Flat:
            for (int i = 0; i < NUM_BANDS; ++i) setBandGain(i, 0.0f);
            break;
        case Preset::VoiceClarity:
            setBandGain(0, 0.0f); setBandGain(1, 3.0f); setBandGain(2, 1.0f);
            setBandGain(3, 0.0f); setBandGain(4, -1.0f); setBandGain(5, 0.0f); setBandGain(6, 0.0f);
            break;
        case Preset::ReduceNoise:
            setBandGain(0, -4.0f); setBandGain(1, -2.0f); setBandGain(2, 0.0f);
            setBandGain(3, 1.0f); setBandGain(4, 2.0f); setBandGain(5, 0.0f); setBandGain(6, -1.0f);
            break;
        case Preset::BassCut:
            setBandGain(0, -8.0f); setBandGain(1, -6.0f); setBandGain(2, -3.0f);
            setBandGain(3, 0.0f); setBandGain(4, 0.0f); setBandGain(5, 0.0f); setBandGain(6, 0.0f);
            break;
        case Preset::PresenceBoost:
            setBandGain(0, 0.0f); setBandGain(1, 0.0f); setBandGain(2, 0.0f);
            setBandGain(3, 2.0f); setBandGain(4, 4.0f); setBandGain(5, 3.0f); setBandGain(6, 1.0f);
            break;
        case Preset::DeEsser:
            setBandGain(0, 0.0f); setBandGain(1, 0.0f); setBandGain(2, 0.0f);
            setBandGain(3, 0.0f); setBandGain(4, -3.0f); setBandGain(5, -6.0f); setBandGain(6, -4.0f);
            break;
    }
}

} // namespace broaudio
