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

// Brickwall lookahead peak limiter.
//
// Scans ahead by `lookAhead` ms to find the true peak, then applies
// gain reduction so the delayed output never exceeds the threshold.
// Attack is instantaneous (the lookahead provides smoothing), release
// is configurable. This guarantees the output cannot clip regardless
// of how hot the input signal is.
class Limiter {
public:
    Limiter(int sampleRate = 44100, int channels = 2);

    void setThreshold(float thresholdDb) { thresholdLin_ = std::pow(10.0f, thresholdDb / 20.0f); }
    void setRelease(float releaseMs) { releaseMs_ = releaseMs; }
    void setLookAhead(float lookAheadMs);
    void setEnabled(bool enabled) { enabled_ = enabled; }

    float getThreshold() const { return 20.0f * std::log10(std::max(thresholdLin_, 1e-12f)); }
    bool isEnabled() const { return enabled_; }

    void process(float* buffer, size_t frames);
    void reset();

private:
    void rebuildLookahead();

    float thresholdLin_ = 0.5012f;  // -6 dBFS
    float releaseMs_ = 50.0f;
    float lookAheadMs_ = 5.0f;
    bool enabled_ = true;

    // Gain envelope (linear, 0-1). Tracks the minimum required gain.
    float envelope_ = 1.0f;
    float releaseCoeff_ = 0.0f;

    // Delay line for the audio signal (interleaved, length = lookahead frames * channels)
    std::vector<float> delayBuf_;
    size_t delayLen_ = 0;   // in samples (frames * channels)
    size_t delayPos_ = 0;

    // Lookahead peak buffer: circular buffer of per-frame peak values
    // used to find the true peak within the lookahead window.
    std::vector<float> peakBuf_;
    size_t peakLen_ = 0;    // in frames
    size_t peakPos_ = 0;

    int sampleRate_;
    int channels_;
};

} // namespace broaudio
