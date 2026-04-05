#pragma once

#include <array>

namespace broaudio {

// 7-band parametric equalizer with stereo support.
// Ported from talkie-qt.
class Equalizer {
public:
    static constexpr int NUM_BANDS = 7;
    static constexpr std::array<float, NUM_BANDS> BAND_FREQUENCIES = {
        60.0f, 170.0f, 350.0f, 1000.0f, 3500.0f, 10000.0f, 16000.0f
    };

    enum class Preset { Flat, VoiceClarity, ReduceNoise, BassCut, PresenceBoost, DeEsser };

    explicit Equalizer(int sampleRate = 44100);

    void setSampleRate(int sampleRate);
    void setMasterGain(float gain);
    void setBandGain(int bandIndex, float gain);
    void setEnabled(bool enabled);
    void reset();

    void process(float* buffer, int numSamples);
    void processStereo(float* leftBuffer, float* rightBuffer, int numSamples);
    void processStereoInterleaved(float* buffer, int numFrames);

    float getMasterGain() const { return masterGain_; }
    float getBandGain(int bandIndex) const;
    bool isEnabled() const { return enabled_; }

    void applyPreset(Preset preset);

private:
    struct BandFilter {
        float frequency = 1000.0f;
        float gain = 0.0f;
        float q = 0.707f;
        float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    };

    void updateFilterCoefficients(int bandIndex);
    float processSample(float input);
    void calculatePeakingEQ(float frequency, float gain, float q,
                            float& a0, float& a1, float& a2,
                            float& b0, float& b1, float& b2);

    int sampleRate_;
    float masterGain_ = 0.0f;
    bool enabled_ = true;
    std::array<BandFilter, NUM_BANDS> bands_;
    std::array<BandFilter, NUM_BANDS> rightBands_;
};

} // namespace broaudio
