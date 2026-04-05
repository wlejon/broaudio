#include "broaudio/synth/wavetable.h"

#include <cmath>
#include <cstring>
#include <numbers>

namespace broaudio {

static constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;

void WavetableBank::addLevel(const float* table, float maxPhaseInc)
{
    if (numLevels_ >= MAX_LEVELS) return;
    std::memcpy(levels_[numLevels_].table, table, TABLE_SIZE * sizeof(float));
    levels_[numLevels_].maxPhaseInc = maxPhaseInc;
    numLevels_++;
}

float WavetableBank::cubicInterp(const float* table, float index)
{
    int i0 = static_cast<int>(index);
    float frac = index - static_cast<float>(i0);

    int im1 = (i0 - 1 + TABLE_SIZE) % TABLE_SIZE;
    int i1  = (i0 + 1) % TABLE_SIZE;
    int i2  = (i0 + 2) % TABLE_SIZE;
    i0 = i0 % TABLE_SIZE;

    float y0 = table[im1];
    float y1 = table[i0];
    float y2 = table[i1];
    float y3 = table[i2];

    // Catmull-Rom spline
    float a = (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * 0.5f;
    float b = (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * 0.5f;
    float c = (-y0 + y2) * 0.5f;
    float d = y1;

    return ((a * frac + b) * frac + c) * frac + d;
}

float WavetableBank::sample(float phase, float phaseInc) const
{
    if (numLevels_ == 0) return 0.0f;

    // Select mipmap level: find the first level whose maxPhaseInc >= our phaseInc
    int level = numLevels_ - 1;  // default to highest (most filtered)
    for (int i = 0; i < numLevels_; i++) {
        if (phaseInc <= levels_[i].maxPhaseInc) {
            level = i;
            break;
        }
    }

    float index = phase * static_cast<float>(TABLE_SIZE);
    if (index < 0.0f) index += static_cast<float>(TABLE_SIZE);

    return cubicInterp(levels_[level].table, index);
}

// Build a table with harmonics up to maxHarmonic using additive synthesis
static void buildAdditiveSaw(float* table, int size, int maxHarmonic)
{
    std::memset(table, 0, size * sizeof(float));
    for (int h = 1; h <= maxHarmonic; h++) {
        float amp = 1.0f / static_cast<float>(h);
        float sign = (h % 2 == 0) ? -1.0f : 1.0f;
        for (int i = 0; i < size; i++) {
            float phase = static_cast<float>(i) / static_cast<float>(size);
            table[i] += sign * amp * std::sin(TWO_PI * h * phase);
        }
    }
    // Normalize: saw amplitude is 2/pi * sum
    float scale = 2.0f / std::numbers::pi_v<float>;
    for (int i = 0; i < size; i++) table[i] *= scale;
}

static void buildAdditiveSquare(float* table, int size, int maxHarmonic)
{
    std::memset(table, 0, size * sizeof(float));
    for (int h = 1; h <= maxHarmonic; h += 2) {  // odd harmonics only
        float amp = 1.0f / static_cast<float>(h);
        for (int i = 0; i < size; i++) {
            float phase = static_cast<float>(i) / static_cast<float>(size);
            table[i] += amp * std::sin(TWO_PI * h * phase);
        }
    }
    float scale = 4.0f / std::numbers::pi_v<float>;
    for (int i = 0; i < size; i++) table[i] *= scale;
}

static void buildAdditiveTriangle(float* table, int size, int maxHarmonic)
{
    std::memset(table, 0, size * sizeof(float));
    for (int h = 1; h <= maxHarmonic; h += 2) {  // odd harmonics only
        float amp = 1.0f / static_cast<float>(h * h);
        float sign = ((h / 2) % 2 == 0) ? 1.0f : -1.0f;
        for (int i = 0; i < size; i++) {
            float phase = static_cast<float>(i) / static_cast<float>(size);
            table[i] += sign * amp * std::sin(TWO_PI * h * phase);
        }
    }
    float scale = 8.0f / (std::numbers::pi_v<float> * std::numbers::pi_v<float>);
    for (int i = 0; i < size; i++) table[i] *= scale;
}

static std::shared_ptr<WavetableBank> buildBank(
    void (*buildFn)(float*, int, int), int sampleRate)
{
    auto bank = std::make_shared<WavetableBank>();
    float table[WavetableBank::TABLE_SIZE];

    // Build one level per octave. Level 0 has the most harmonics (lowest frequencies),
    // level N has fewer (for high frequencies where harmonics would alias).
    // Base frequency for level i: sampleRate / (2^(MAX_LEVELS - i))
    for (int i = 0; i < WavetableBank::MAX_LEVELS; i++) {
        // At this level, the max fundamental frequency before we'd need the next level
        float maxFreq = static_cast<float>(sampleRate) / std::pow(2.0f, static_cast<float>(WavetableBank::MAX_LEVELS - i));
        float maxPhaseInc = maxFreq / static_cast<float>(sampleRate);

        // Max harmonic: Nyquist / maxFreq
        int maxHarmonic = static_cast<int>(0.5f / maxPhaseInc);
        if (maxHarmonic < 1) maxHarmonic = 1;

        buildFn(table, WavetableBank::TABLE_SIZE, maxHarmonic);
        bank->addLevel(table, maxPhaseInc);
    }

    return bank;
}

std::shared_ptr<WavetableBank> WavetableBank::createSaw(int sampleRate)
{
    return buildBank(buildAdditiveSaw, sampleRate);
}

std::shared_ptr<WavetableBank> WavetableBank::createSquare(int sampleRate)
{
    return buildBank(buildAdditiveSquare, sampleRate);
}

std::shared_ptr<WavetableBank> WavetableBank::createTriangle(int sampleRate)
{
    return buildBank(buildAdditiveTriangle, sampleRate);
}

std::shared_ptr<WavetableBank> WavetableBank::createFromWaveform(
    const float* waveform, int numSamples, int sampleRate)
{
    auto bank = std::make_shared<WavetableBank>();

    // Analyze the input waveform into harmonics via DFT
    // (We only need the magnitudes and phases of harmonics, not a full FFT)
    int maxPossibleHarmonic = numSamples / 2;
    std::vector<float> cosCoeffs(maxPossibleHarmonic + 1);
    std::vector<float> sinCoeffs(maxPossibleHarmonic + 1);

    float invN = 1.0f / static_cast<float>(numSamples);
    for (int h = 1; h <= maxPossibleHarmonic; h++) {
        float cosSum = 0.0f, sinSum = 0.0f;
        for (int i = 0; i < numSamples; i++) {
            float angle = TWO_PI * h * static_cast<float>(i) * invN;
            cosSum += waveform[i] * std::cos(angle);
            sinSum += waveform[i] * std::sin(angle);
        }
        cosCoeffs[h] = cosSum * 2.0f * invN;
        sinCoeffs[h] = sinSum * 2.0f * invN;
    }

    float table[TABLE_SIZE];

    for (int lvl = 0; lvl < MAX_LEVELS; lvl++) {
        float maxFreq = static_cast<float>(sampleRate) / std::pow(2.0f, static_cast<float>(MAX_LEVELS - lvl));
        float maxPhaseInc = maxFreq / static_cast<float>(sampleRate);
        int maxHarmonic = static_cast<int>(0.5f / maxPhaseInc);
        if (maxHarmonic > maxPossibleHarmonic) maxHarmonic = maxPossibleHarmonic;
        if (maxHarmonic < 1) maxHarmonic = 1;

        // Resynthesize with limited harmonics
        std::memset(table, 0, TABLE_SIZE * sizeof(float));
        for (int h = 1; h <= maxHarmonic; h++) {
            for (int i = 0; i < TABLE_SIZE; i++) {
                float phase = static_cast<float>(i) / static_cast<float>(TABLE_SIZE);
                float angle = TWO_PI * h * phase;
                table[i] += cosCoeffs[h] * std::cos(angle) + sinCoeffs[h] * std::sin(angle);
            }
        }

        bank->addLevel(table, maxPhaseInc);
    }

    return bank;
}

} // namespace broaudio
