#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace broaudio {

// A band-limited wavetable with mipmap levels per octave.
// Each mipmap level contains a single-cycle waveform with harmonics
// limited to those below Nyquist for that octave range.
//
// Usage:
//   auto bank = WavetableBank::createSaw(44100);
//   float sample = bank->sample(phase, phaseInc);
class WavetableBank {
public:
    static constexpr int TABLE_SIZE = 2048;  // samples per table
    static constexpr int MAX_LEVELS = 12;    // covers ~20Hz to ~20kHz

    // Factory methods — create band-limited tables for standard waveforms
    static std::shared_ptr<WavetableBank> createSaw(int sampleRate);
    static std::shared_ptr<WavetableBank> createSquare(int sampleRate);
    static std::shared_ptr<WavetableBank> createTriangle(int sampleRate);

    // Create from arbitrary single-cycle waveform (TABLE_SIZE samples, normalized 0-1 phase)
    // Harmonics above Nyquist are removed per octave level via additive resynthesis.
    static std::shared_ptr<WavetableBank> createFromWaveform(
        const float* waveform, int numSamples, int sampleRate);

    // Sample the wavetable at the given phase (0-1) and phaseInc (freq/sampleRate).
    // Automatically selects the appropriate mipmap level and uses cubic interpolation.
    float sample(float phase, float phaseInc) const;

    int numLevels() const { return numLevels_; }

    // Add a mipmap level. table must have TABLE_SIZE samples.
    // maxPhaseInc is the highest freq/sampleRate this level covers.
    void addLevel(const float* table, float maxPhaseInc);

private:
    struct Level {
        float table[TABLE_SIZE];
        float maxPhaseInc;  // upper bound of phaseInc for this level
    };

    Level levels_[MAX_LEVELS];
    int numLevels_ = 0;

    static float cubicInterp(const float* table, float index);
};

} // namespace broaudio
