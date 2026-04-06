#pragma once

#include "broaudio/dsp/smoother.h"
#include "broaudio/spatial/listener.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace broaudio {

struct AudioClip {
    int id = 0;
    int channels = 1;              // 1 = mono, 2 = stereo (interleaved)
    std::vector<float> samples;    // length = numFrames * channels

    int numFrames() const { return channels > 0 ? static_cast<int>(samples.size()) / channels : 0; }
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
    std::atomic<int> sendBusId{-1};      // aux send target (-1 = none)
    std::atomic<float> sendAmount{0.0f}; // aux send level (0-1)
    std::atomic<float> rate{1.0f};
    std::atomic<bool> playing{false};
    std::atomic<bool> looping{false};
    std::atomic<bool> active{true};

    // Parameter smoothers (audio thread only)
    Smoother smoothGain;
    Smoother smoothPan;

    // Spatial source and directional filter (audio-thread only)
    SpatialSource spatial;
    SpatialFilter spatialFilter;
};

} // namespace broaudio
