#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace broaudio {

struct AudioClip {
    int id = 0;
    std::vector<float> samples;
};

struct ClipPlayback {
    int id = 0;
    int clipId = 0;
    std::atomic<int> regionStart{0};
    std::atomic<int> regionEnd{0}; // 0 = full clip
    std::atomic<uint64_t> playPos{0};  // fixed-point: upper bits = sample, lower 16 = fraction
    std::atomic<float> gain{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<int> busId{0};           // target mix bus (0 = master)
    std::atomic<float> rate{1.0f};
    std::atomic<bool> playing{false};
    std::atomic<bool> looping{false};
    std::atomic<bool> active{true};
};

} // namespace broaudio
