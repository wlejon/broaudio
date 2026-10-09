#pragma once

// Streaming F32 rate converter that keeps filter state across chunks (file
// streams, mic taps, loopback capture). Private to broaudio; the public
// headers only forward-declare it, so the implementation (SDL3's audio
// stream converter today) never reaches a consumer's include path.

#include <memory>

namespace broaudio {

class StreamResampler {
public:
    // Null when the converter cannot be created.
    static std::unique_ptr<StreamResampler> create(int channels, int srcRate, int dstRate);
    ~StreamResampler();

    StreamResampler(const StreamResampler&) = delete;
    StreamResampler& operator=(const StreamResampler&) = delete;

    // Interleaved frames in at srcRate.
    bool put(const float* frames, int numFrames);
    // Frames ready at dstRate.
    int available() const;
    // Up to maxFrames out; returns frames written (0 when none / on error).
    int get(float* out, int maxFrames);
    // Push the tail through (end of input).
    void flush();
    // Drop everything buffered (seek).
    void clear();

private:
    StreamResampler() = default;
    void* stream_ = nullptr;
    int frameBytes_ = 0;
};

} // namespace broaudio
