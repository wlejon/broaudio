#include "broaudio/dsp/chorus.h"

#include <cmath>
#include <numbers>

namespace broaudio {

static constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;

void Chorus::init(int sampleRate)
{
    sampleRate_ = sampleRate;
    // Max delay = baseDelay + depth, with headroom. 100ms covers all practical settings.
    bufferSize_ = static_cast<int>(0.1f * sampleRate) + 1;
    bufferL_.assign(bufferSize_, 0.0f);
    bufferR_.assign(bufferSize_, 0.0f);
    writePos_ = 0;
    phaseL_ = 0.0f;
    phaseR_ = 0.25f;
}

void Chorus::processStereo(float* buf, int numFrames)
{
    float sr = static_cast<float>(sampleRate_);
    float phaseInc = rate / sr;

    for (int i = 0; i < numFrames; i++) {
        float inL = buf[i * 2];
        float inR = buf[i * 2 + 1];

        // LFO: sine modulation of delay time
        float modL = std::sin(phaseL_ * TWO_PI) * depth * sr;
        float modR = std::sin(phaseR_ * TWO_PI) * depth * sr;

        float delayL = baseDelay * sr + modL;
        float delayR = baseDelay * sr + modR;

        // Clamp delay to buffer bounds
        if (delayL < 1.0f) delayL = 1.0f;
        if (delayR < 1.0f) delayR = 1.0f;
        if (delayL >= bufferSize_ - 1) delayL = static_cast<float>(bufferSize_ - 2);
        if (delayR >= bufferSize_ - 1) delayR = static_cast<float>(bufferSize_ - 2);

        // Read with linear interpolation
        float readPosL = static_cast<float>(writePos_) - delayL;
        if (readPosL < 0.0f) readPosL += static_cast<float>(bufferSize_);
        int idxL = static_cast<int>(readPosL);
        float fracL = readPosL - static_cast<float>(idxL);
        float wetL = bufferL_[idxL % bufferSize_] * (1.0f - fracL)
                   + bufferL_[(idxL + 1) % bufferSize_] * fracL;

        float readPosR = static_cast<float>(writePos_) - delayR;
        if (readPosR < 0.0f) readPosR += static_cast<float>(bufferSize_);
        int idxR = static_cast<int>(readPosR);
        float fracR = readPosR - static_cast<float>(idxR);
        float wetR = bufferR_[idxR % bufferSize_] * (1.0f - fracR)
                   + bufferR_[(idxR + 1) % bufferSize_] * fracR;

        // Write into delay buffer (with feedback for flanger)
        bufferL_[writePos_] = inL + wetL * feedback;
        bufferR_[writePos_] = inR + wetR * feedback;

        // Mix
        buf[i * 2]     = inL * (1.0f - mix) + wetL * mix;
        buf[i * 2 + 1] = inR * (1.0f - mix) + wetR * mix;

        writePos_ = (writePos_ + 1) % bufferSize_;
        phaseL_ += phaseInc;
        if (phaseL_ >= 1.0f) phaseL_ -= 1.0f;
        phaseR_ += phaseInc;
        if (phaseR_ >= 1.0f) phaseR_ -= 1.0f;
    }
}

} // namespace broaudio
