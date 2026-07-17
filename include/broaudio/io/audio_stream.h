#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace broaudio {

// Incremental (pull-based) audio file decoder — the disk-streaming counterpart
// of loadAudioFile(). Opens WAV, FLAC, MP3, or Ogg Vorbis and decodes
// interleaved float32 PCM in chunks instead of the whole file at once, so
// arbitrarily large files play without being resident in RAM.
//
// Not thread-safe: one AudioFileStream is owned and driven by exactly one
// thread (the file-stream worker). Decoding does disk I/O — never call from
// the real-time audio thread.
class AudioFileStream {
public:
    AudioFileStream();
    ~AudioFileStream();
    AudioFileStream(const AudioFileStream&) = delete;
    AudioFileStream& operator=(const AudioFileStream&) = delete;

    // Open a file for streaming. Format is detected from the leading bytes
    // (same sniff as loadAudioFile). Returns false on failure with error()
    // set to an actionable message when the cause is known.
    bool open(const char* path);
    void close();
    bool isOpen() const;

    const std::string& error() const { return error_; }

    int channels() const { return channels_; }
    int sampleRate() const { return sampleRate_; }

    // Total frame count, or 0 when the container cannot say without scanning
    // the whole file (MP3).
    uint64_t totalFrames() const { return totalFrames_; }

    // Decode up to maxFrames interleaved frames into dst (needs room for
    // maxFrames * channels() floats). Returns frames decoded; 0 at end of
    // stream or on decode failure.
    int readFrames(float* dst, int maxFrames);

    // Rewind to the first frame (seamless looping). Returns false if the
    // backend cannot seek.
    bool seekToStart();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
    int channels_ = 0;
    int sampleRate_ = 0;
    uint64_t totalFrames_ = 0;
};

} // namespace broaudio
