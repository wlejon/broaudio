#pragma once

// MP4 / M4A audio: minimp4 (CC0) for the sample tables, the tag reader's box
// walker (src/io/tags/mp4.cpp) for the edit list, and the platform's AAC
// decoder (aac_decoder.h) for the audio.
//
//  - The first sound track is decoded; it must be AAC (AAC-LC, and HE-AAC
//    where the platform decoder handles it). ALAC and the rest fail with a
//    clear error, as does every MP4 on a platform with no AAC decoder.
//  - The edit list's media_time (the encoder's priming, 1024 or 2112
//    frames) is dropped and its segment duration is the length, so the
//    output is the audio the encoder was given, frame for frame.
//  - Seeking finds the packet holding the target from the sample table,
//    decodes from two packets before it (the MDCT overlap converges) and
//    drops the frames before the target: sample-exact.
//
// Reads go through a random-access byte source (a file or a memory span):
// the sample table is held, never the file. Not thread-safe: one owner.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace broaudio {

// Whether bytes from the start of a file look like an ISO media file ("ftyp" box first).
bool looksLikeMp4(const uint8_t* head, size_t size);

class Mp4AacDecoder {
public:
    Mp4AacDecoder();
    ~Mp4AacDecoder();
    Mp4AacDecoder(const Mp4AacDecoder&) = delete;
    Mp4AacDecoder& operator=(const Mp4AacDecoder&) = delete;

    // Open from a path or memory (which must outlive the decoder). On
    // failure returns false with *error set to a readable reason.
    bool openFile(const char* path, std::string* error);
    bool openMemory(const uint8_t* data, size_t size, std::string* error);

    int channels() const;
    int sampleRate() const;
    // Length in frames at sampleRate(), priming and padding excluded.
    uint64_t totalFrames() const;

    // Decode up to maxFrames interleaved frames into dst (room for
    // maxFrames * channels() floats). Returns frames written; 0 at the end.
    int readFrames(float* dst, int maxFrames);

    // Seek to an absolute frame. False when it is past the end.
    bool seekToFrame(uint64_t frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace broaudio
