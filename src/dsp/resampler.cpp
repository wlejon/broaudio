#include "broaudio/dsp/resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace broaudio {

// ---------------------------------------------------------------------------
// Kaiser window helpers
// ---------------------------------------------------------------------------

// Modified Bessel function of the first kind, order 0 (series expansion).
static double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    double halfX = x * 0.5;
    for (int k = 1; k < 30; k++) {
        term *= (halfX / k) * (halfX / k);
        sum += term;
        if (term < 1e-20 * sum) break;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Polyphase sinc resampler
//
// For each output sample, computes a windowed sinc kernel centered at the
// exact fractional input position. The kernel taps are pre-computed per
// sub-sample phase and stored in a lookup table for efficiency.
// ---------------------------------------------------------------------------

static constexpr int HALF_TAPS = 16;               // taps on each side of center
static constexpr int KERNEL_SIZE = 2 * HALF_TAPS;  // total taps per output sample
static constexpr int NUM_PHASES = 512;              // sub-sample resolution
static constexpr double KAISER_BETA = 7.0;          // ~70 dB stopband attenuation

// Build polyphase filter table.
// Table layout: table[phase * KERNEL_SIZE + tap], phase in [0, NUM_PHASES).
// Phase p corresponds to fractional offset p / NUM_PHASES.
//
// For phase p, tap t (0-based), the sinc is evaluated at:
//   x = (t - HALF_TAPS + 1) - frac,  where frac = p / NUM_PHASES
// This centers the kernel so that tap HALF_TAPS-1 is at offset -frac
// (i.e., the nearest input sample to the left of the target position).
static std::vector<float> buildFilterTable(double cutoff)
{
    std::vector<float> table(NUM_PHASES * KERNEL_SIZE);
    double invI0Beta = 1.0 / besselI0(KAISER_BETA);

    for (int p = 0; p < NUM_PHASES; p++) {
        double frac = static_cast<double>(p) / NUM_PHASES;
        float* row = table.data() + p * KERNEL_SIZE;
        double sum = 0.0;

        for (int t = 0; t < KERNEL_SIZE; t++) {
            // Distance from this tap to the ideal interpolation point
            double x = static_cast<double>(t - HALF_TAPS) + (1.0 - frac);

            // Windowed sinc
            double sincVal;
            if (std::fabs(x) < 1e-12)
                sincVal = cutoff;
            else
                sincVal = std::sin(std::numbers::pi * cutoff * x) / (std::numbers::pi * x);

            // Kaiser window over the kernel span
            double wPos = static_cast<double>(t) / static_cast<double>(KERNEL_SIZE - 1);
            double wArg = 2.0 * wPos - 1.0;
            double kaiser = besselI0(KAISER_BETA * std::sqrt(std::max(0.0, 1.0 - wArg * wArg)))
                            * invI0Beta;

            row[t] = static_cast<float>(sincVal * kaiser);
            sum += row[t];
        }

        // Normalize so each phase has unity DC gain
        if (std::fabs(sum) > 1e-12) {
            float invSum = static_cast<float>(1.0 / sum);
            for (int t = 0; t < KERNEL_SIZE; t++)
                row[t] *= invSum;
        }
    }

    return table;
}

std::vector<float> resample(const float* input, int inputFrames, int inputChannels,
                            int inputRate, int outputRate)
{
    if (!input || inputFrames <= 0 || inputChannels <= 0 ||
        inputRate <= 0 || outputRate <= 0)
        return {};

    // No conversion needed
    if (inputRate == outputRate) {
        size_t totalSamples = static_cast<size_t>(inputFrames) * inputChannels;
        return std::vector<float>(input, input + totalSamples);
    }

    // Anti-aliasing cutoff: filter at the lower of the two Nyquist frequencies
    double ratio = static_cast<double>(outputRate) / static_cast<double>(inputRate);
    double cutoff = std::min(1.0, ratio);
    cutoff *= 0.95; // slight rolloff to keep transition band inside Nyquist

    auto table = buildFilterTable(cutoff);

    int outputFrames = static_cast<int>(std::ceil(static_cast<double>(inputFrames) * ratio));
    size_t totalOutputSamples = static_cast<size_t>(outputFrames) * inputChannels;
    std::vector<float> output(totalOutputSamples, 0.0f);

    // Process each channel independently (deinterleave → resample → reinterleave)
    std::vector<float> chanIn(inputFrames);
    std::vector<float> chanOut(outputFrames);

    double step = static_cast<double>(inputRate) / static_cast<double>(outputRate);

    for (int ch = 0; ch < inputChannels; ch++) {
        // Deinterleave
        for (int i = 0; i < inputFrames; i++)
            chanIn[i] = input[i * inputChannels + ch];

        for (int outIdx = 0; outIdx < outputFrames; outIdx++) {
            double inputPos = outIdx * step;
            int intPos = static_cast<int>(std::floor(inputPos));
            double frac = inputPos - intPos;

            // Select polyphase sub-filter from fractional position
            int phase = static_cast<int>(frac * NUM_PHASES);
            if (phase >= NUM_PHASES) phase = NUM_PHASES - 1;

            const float* coeffs = table.data() + phase * KERNEL_SIZE;

            // Convolve: kernel is centered so tap HALF_TAPS aligns with intPos+1
            // and tap HALF_TAPS-1 aligns with intPos (the sample just left of target)
            float sum = 0.0f;
            int base = intPos - HALF_TAPS + 1;

            for (int t = 0; t < KERNEL_SIZE; t++) {
                int srcIdx = base + t;
                if (srcIdx < 0) srcIdx = 0;
                else if (srcIdx >= inputFrames) srcIdx = inputFrames - 1;
                sum += chanIn[srcIdx] * coeffs[t];
            }

            chanOut[outIdx] = sum;
        }

        // Reinterleave
        for (int i = 0; i < outputFrames; i++)
            output[i * inputChannels + ch] = chanOut[i];
    }

    return output;
}

} // namespace broaudio
