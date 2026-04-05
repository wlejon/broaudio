#pragma once

#include <atomic>

namespace broaudio {

struct FilterParams {
    std::atomic<bool> enabled{false};
    std::atomic<int> type{0};
    std::atomic<float> frequency{1000.0f};
    std::atomic<float> Q{1.0f};
    std::atomic<float> gainDB{0.0f};
    std::atomic<bool> allocated{false};
    std::atomic<uint32_t> version{0};
};

struct DelayParams {
    std::atomic<bool> enabled{false};
    std::atomic<float> time{0.3f};
    std::atomic<float> feedback{0.3f};
    std::atomic<float> mix{0.5f};
    std::atomic<uint32_t> version{0};
};

struct CompressorParams {
    std::atomic<bool> enabled{false};
    std::atomic<float> threshold{0.7f};
    std::atomic<float> ratio{4.0f};
    std::atomic<float> attackMs{1.0f};
    std::atomic<float> releaseMs{100.0f};
    std::atomic<int> sidechainBusId{-1}; // -1 = self-detect, >= 0 = use that bus's signal
    std::atomic<uint32_t> version{0};
};

struct ReverbParams {
    std::atomic<bool> enabled{false};
    std::atomic<float> roomSize{0.85f};
    std::atomic<float> damping{0.5f};
    std::atomic<float> mix{0.3f};
    std::atomic<uint32_t> version{0};
};

struct ChorusParams {
    std::atomic<bool> enabled{false};
    std::atomic<float> rate{0.5f};       // LFO Hz
    std::atomic<float> depth{0.005f};    // seconds
    std::atomic<float> mix{0.5f};
    std::atomic<float> feedback{0.0f};   // 0=chorus, >0=flanger
    std::atomic<float> baseDelay{0.01f}; // seconds
    std::atomic<uint32_t> version{0};
};

enum class DistortionMode : uint8_t {
    SoftClip,   // tanh saturation
    HardClip,   // flat clamp
    Foldback,   // wave folding
    Bitcrush,   // bit depth + sample rate reduction
};

struct DistortionParams {
    std::atomic<bool> enabled{false};
    std::atomic<int> mode{static_cast<int>(DistortionMode::SoftClip)};
    std::atomic<float> drive{1.0f};     // 1.0 = unity, higher = more distortion
    std::atomic<float> mix{1.0f};       // wet/dry (0 = dry, 1 = full wet)
    std::atomic<float> outputGain{1.0f}; // post-distortion gain compensation
    std::atomic<float> crushBits{16.0f}; // bit depth for Bitcrush mode (1-16)
    std::atomic<float> crushRate{1.0f};  // sample rate factor for Bitcrush (0.01-1.0)
    std::atomic<uint32_t> version{0};
};

struct EqualizerParams {
    std::atomic<bool> enabled{false};
    std::atomic<float> bandGains[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::atomic<float> masterGain{0.0f};
    std::atomic<uint32_t> version{0};
};

} // namespace broaudio
