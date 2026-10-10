#pragma once

// Ogg Opus decoding over libopus (RFC 7845), without opusfile: a small Ogg
// page/packet demuxer feeding opus_multistream_decode_float.
//
//  - OpusHead: channel count, pre-skip, output gain (applied by the decoder),
//    channel mapping family 0 (mono/stereo) and 1/255 (multistream).
//  - Granule positions give the exact length (last page's granule minus the
//    pre-skip and the first page's start) and drive seeking: bisection over
//    the file for the page before the target, 80 ms of pre-roll decoded and
//    discarded so the decoder has converged, then sample-exact trimming.
//  - Output is interleaved float32 at 48 kHz (Opus's native rate).
//
// Reads go through a random-access byte source (a FILE* or a memory span),
// so a disk stream holds one page plus one decoded packet in memory, never
// the file. Only the first logical Opus stream is decoded; a chained stream
// ends at its first link.
//
// Compiled only when broaudio links libopus (BROAUDIO_HAS_OPUS). Not
// thread-safe: one decoder per thread.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace broaudio {

class OggOpusDecoder {
public:
    OggOpusDecoder();
    ~OggOpusDecoder();
    OggOpusDecoder(const OggOpusDecoder&) = delete;
    OggOpusDecoder& operator=(const OggOpusDecoder&) = delete;

    // Open from a path or from memory (the memory must outlive the decoder).
    // On failure returns false with *error set to a readable reason.
    bool openFile(const char* path, std::string* error);
    bool openMemory(const uint8_t* data, size_t size, std::string* error);

    int channels() const;
    static constexpr int sampleRate() { return 48000; }
    // Exact PCM length in frames (48 kHz); 0 when the file has no audio.
    uint64_t totalFrames() const;

    // Decode up to maxFrames interleaved frames into dst (room for
    // maxFrames * channels() floats). Returns frames written; 0 at the end.
    int readFrames(float* dst, int maxFrames);

    // Seek to an absolute PCM frame (48 kHz). Returns false when the target
    // cannot be reached; the position is then unspecified.
    bool seekToFrame(uint64_t frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace broaudio
