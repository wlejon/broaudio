#pragma once

// AAC packets to PCM through the platform's decoder: Media Foundation's AAC
// decoder MFT on Windows, AudioToolbox's AudioConverter on macOS. broaudio
// ships no AAC decoder of its own, so other platforms have none
// (BROAUDIO_HAS_AAC is 0 there and makePlatformAacDecoder says so).
//
// One raw AAC access unit in (as an MP4 sample holds it, no ADTS header),
// interleaved float32 out. Not thread-safe: one owner at a time (it may move
// between threads, as a disk stream's decoder does).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace broaudio {

class AacDecoder {
public:
    virtual ~AacDecoder() = default;

    // Decode one access unit, appending interleaved float32 frames to `out`.
    // False on a decode error (the caller may skip the packet).
    virtual bool decode(const uint8_t* packet, size_t size, std::vector<float>& out) = 0;

    // Drop the decoder's state (overlap, buffered output) before a seek.
    virtual void reset() = 0;

    // The end of the stream: append what the decoder still holds (the last
    // packet's audio, on a decoder with latency()).
    virtual bool drain(std::vector<float>& out) = 0;

    // Frames of output the decoder lags its input by, beyond the AAC
    // overlap every decoder has (which the file's priming already counts):
    // Media Foundation's decoder answers packet k with packet k-1's audio,
    // so its first output is a frame of silence and its last packet's audio
    // only comes out on drain(). Known after the first packet that produced
    // audio.
    virtual int latency() const = 0;

    // The output format. Known after the first packet that produced audio
    // (HE-AAC's SBR doubles the rate the AudioSpecificConfig states).
    virtual int sampleRate() const = 0;
    virtual int channels() const = 0;
};

// The AudioSpecificConfig's core fields.
struct AacConfig {
    int objectType = 0;      // 2 = AAC-LC, 5 = HE-AAC (SBR), 29 = HE-AACv2 (PS)
    int sampleRate = 0;      // the core rate
    int channels = 0;        // channelConfiguration (0 = in the bitstream's PCE)
    int extSampleRate = 0;   // SBR output rate when explicitly signalled
};
bool parseAudioSpecificConfig(const uint8_t* asc, size_t size, AacConfig& out);

// A decoder for the stream `asc` describes, or null with *error set.
std::unique_ptr<AacDecoder> makePlatformAacDecoder(const uint8_t* asc, size_t ascSize,
                                                   std::string* error);

} // namespace broaudio
