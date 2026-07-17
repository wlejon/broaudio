#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace broaudio {

// Decoded audio data returned by loadAudioFile / loadAudioFileFromMemory.
struct AudioFileData {
    std::vector<float> samples;  // interleaved float32 PCM
    int channels = 0;            // 1 = mono, 2 = stereo
    int sampleRate = 0;
    int numFrames = 0;           // total frames (samples.size() / channels)

    // Human-readable reason when valid() is false and the cause is known
    // (e.g. "Ogg Vorbis is not supported..."). Empty on success and on
    // generic decode failures.
    std::string error;

    bool valid() const { return numFrames > 0 && channels > 0; }
};

// Load audio from a file path.
// Format is detected by extension: .wav, .flac, .mp3. For .ogg/.opus the
// codec inside the Ogg container is sniffed from the first packet: Opus
// decodes only when built with BROAUDIO_HAS_OPUS; Ogg Vorbis is not
// supported (a clear `error` is set in both unsupported cases).
// Returns an invalid AudioFileData on failure (valid() == false).
AudioFileData loadAudioFile(const char* path);

// Load audio from a memory buffer.
// Format is detected by header magic bytes (same codec rules as above).
AudioFileData loadAudioFileFromMemory(const uint8_t* data, size_t size);

// Export interleaved float32 PCM to a WAV file (32-bit float format).
// Returns true on success.
bool saveWav(const char* path, const float* samples, int numFrames, int channels, int sampleRate);

} // namespace broaudio
