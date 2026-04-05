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
    std::atomic<bool> enabled{true};
    std::atomic<float> threshold{0.7f};
    std::atomic<float> ratio{4.0f};
    std::atomic<float> attackMs{1.0f};
    std::atomic<float> releaseMs{100.0f};
    std::atomic<uint32_t> version{0};
};

} // namespace broaudio
