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

    // Streaming source (live PCM push — voice chat, network audio). When true,
    // `samples` is a fixed-capacity ring of `ringFrames` frames and `writeFrames`
    // is the monotonic count of frames ever pushed. SPSC: the producer (main
    // thread, pushStreamSamples) writes ring slots then release-stores
    // writeFrames; the audio thread acquire-loads writeFrames and reads only
    // slots below it. The mixer outputs silence on underrun and skips ahead on
    // overrun to bound latency.
    bool streaming = false;
    int  ringFrames = 0;
    std::atomic<uint64_t> writeFrames{0};

    // Disk-streamed sources (createStreamFromFile) only:
    // streamEnded — producer sets it (release) once the decoder hit EOF (not
    // looping) and every decoded frame is in the ring, so post-stream silence
    // is not miscounted as starvation. streamUnderrunFrames — audio-thread
    // counter of silent frames emitted because the ring ran dry mid-stream;
    // read from the control plane via Engine::getStreamStats.
    std::atomic<bool> streamEnded{false};
    std::atomic<uint64_t> streamUnderrunFrames{0};

    int numFrames() const { return channels > 0 ? static_cast<int>(samples.size()) / channels : 0; }
};

// Append interleaved frames to a streaming clip's ring and release-publish the
// new writeFrames. Single-producer: exactly one thread may call this per clip
// (main thread for live streams, the file-stream worker for disk streams).
// A push larger than the ring keeps only the most recent capacity. Returns
// frames written.
int pushStreamFrames(AudioClip& clip, const float* samples, int numFrames);

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
    // Sample-accurate scheduled start: absolute engine sample index (cf.
    // Engine::currentTime) at which mixing begins. 0 = start immediately (the
    // plain playClip default). Set by playClipAt so a streamed sequence of
    // clips joins gaplessly on the audio clock instead of via main-thread timers.
    std::atomic<uint64_t> startSample{0};

    // Parameter smoothers (audio thread only)
    Smoother smoothGain;
    Smoother smoothPan;

    // Spatial source and directional filter (audio-thread only)
    SpatialSource spatial;
    SpatialFilter spatialFilter;
};

} // namespace broaudio
