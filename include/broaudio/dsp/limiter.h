#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace broaudio {

// Soft limiter — tanh-based, transparent below threshold
inline float softLimit(float x) {
    constexpr float thresh = 0.8f;
    if (x > thresh) {
        float over = x - thresh;
        return thresh + (1.0f - thresh) * (1.0f - std::exp(-over / (1.0f - thresh)));
    }
    if (x < -thresh) {
        float over = -x - thresh;
        return -(thresh + (1.0f - thresh) * (1.0f - std::exp(-over / (1.0f - thresh))));
    }
    return x;
}

// Lookahead peak limiter with attack/release envelope.
// Ported from talkie-qt.
class Limiter {
public:
    Limiter(int sampleRate = 44100, int channels = 2);

    void setThreshold(float thresholdDb) { threshold_ = thresholdDb; }
    void setRatio(float ratio) { ratio_ = ratio; }
    void setAttack(float attackMs) { attack_ = attackMs; }
    void setRelease(float releaseMs) { release_ = releaseMs; }
    void setLookAhead(float lookAheadMs) { lookAhead_ = lookAheadMs; updateDelayBuffer(); }
    void setEnabled(bool enabled) { enabled_ = enabled; }

    float getThreshold() const { return threshold_; }
    float getRatio() const { return ratio_; }
    bool isEnabled() const { return enabled_; }

    void process(float* buffer, size_t frames);
    void reset();

private:
    void updateCoefficients();
    void updateDelayBuffer();
    float threshold_ = -6.0f;
    float ratio_ = 4.0f;
    float attack_ = 1.0f;
    float release_ = 50.0f;
    float lookAhead_ = 5.0f;
    bool enabled_ = true;

    float envelope_ = 0.0f;
    float attackCoeff_ = 0.0f;
    float releaseCoeff_ = 0.0f;

    std::vector<float> delayBuffer_;
    size_t delayWritePos_ = 0;
    size_t delayReadPos_ = 0;
    size_t delayLength_ = 0;

    int sampleRate_;
    int channels_;
};

} // namespace broaudio
