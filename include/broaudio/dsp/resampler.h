#pragma once

#include <vector>

namespace broaudio {

// Polyphase sinc resampler for offline (non-realtime) sample rate conversion.
// Uses a windowed sinc filter (Kaiser window) decomposed into polyphase
// sub-filters for efficient arbitrary-ratio conversion.
//
// Usage:
//   auto output = resample(input.data(), numFrames, channels, 48000, 44100);
//
// The output vector contains interleaved float samples at the target rate.
std::vector<float> resample(const float* input, int inputFrames, int inputChannels,
                            int inputRate, int outputRate);

} // namespace broaudio
