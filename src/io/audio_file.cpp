#include "broaudio/io/audio_file.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// dr_libs implementations (compiled once here)
// ---------------------------------------------------------------------------

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

// ---------------------------------------------------------------------------
// Optional Opus/OGG support via opusfile
// ---------------------------------------------------------------------------

#if defined(BROAUDIO_HAS_OPUS) && BROAUDIO_HAS_OPUS
#include <opusfile.h>
#endif

namespace broaudio {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static AudioFileData makeResult(float* data, drwav_uint64 frames, unsigned int channels, unsigned int sampleRate)
{
    if (!data || frames == 0) return {};

    AudioFileData result;
    result.channels = static_cast<int>(channels);
    result.sampleRate = static_cast<int>(sampleRate);
    result.numFrames = static_cast<int>(frames);
    result.samples.assign(data, data + frames * channels);
    drwav_free(data, nullptr);
    return result;
}

static std::string extensionLower(const char* path)
{
    std::string p(path);
    auto dot = p.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = p.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

static AudioFileData loadWav(const char* path)
{
    unsigned int channels, sampleRate;
    drwav_uint64 totalFrames;
    float* data = drwav_open_file_and_read_pcm_frames_f32(path, &channels, &sampleRate, &totalFrames, nullptr);
    return makeResult(data, totalFrames, channels, sampleRate);
}

static AudioFileData loadWavMemory(const uint8_t* data, size_t size)
{
    unsigned int channels, sampleRate;
    drwav_uint64 totalFrames;
    float* pcm = drwav_open_memory_and_read_pcm_frames_f32(data, size, &channels, &sampleRate, &totalFrames, nullptr);
    return makeResult(pcm, totalFrames, channels, sampleRate);
}

// ---------------------------------------------------------------------------
// FLAC
// ---------------------------------------------------------------------------

static AudioFileData loadFlac(const char* path)
{
    unsigned int channels, sampleRate;
    drflac_uint64 totalFrames;
    float* data = drflac_open_file_and_read_pcm_frames_f32(path, &channels, &sampleRate, &totalFrames, nullptr);
    if (!data || totalFrames == 0) return {};

    AudioFileData result;
    result.channels = static_cast<int>(channels);
    result.sampleRate = static_cast<int>(sampleRate);
    result.numFrames = static_cast<int>(totalFrames);
    result.samples.assign(data, data + totalFrames * channels);
    drflac_free(data, nullptr);
    return result;
}

static AudioFileData loadFlacMemory(const uint8_t* data, size_t size)
{
    unsigned int channels, sampleRate;
    drflac_uint64 totalFrames;
    float* pcm = drflac_open_memory_and_read_pcm_frames_f32(data, size, &channels, &sampleRate, &totalFrames, nullptr);
    if (!pcm || totalFrames == 0) return {};

    AudioFileData result;
    result.channels = static_cast<int>(channels);
    result.sampleRate = static_cast<int>(sampleRate);
    result.numFrames = static_cast<int>(totalFrames);
    result.samples.assign(pcm, pcm + totalFrames * channels);
    drflac_free(pcm, nullptr);
    return result;
}

// ---------------------------------------------------------------------------
// MP3
// ---------------------------------------------------------------------------

static AudioFileData loadMp3(const char* path)
{
    drmp3_config config;
    drmp3_uint64 totalFrames;
    float* data = drmp3_open_file_and_read_pcm_frames_f32(path, &config, &totalFrames, nullptr);
    if (!data || totalFrames == 0) return {};

    AudioFileData result;
    result.channels = static_cast<int>(config.channels);
    result.sampleRate = static_cast<int>(config.sampleRate);
    result.numFrames = static_cast<int>(totalFrames);
    result.samples.assign(data, data + totalFrames * config.channels);
    drmp3_free(data, nullptr);
    return result;
}

static AudioFileData loadMp3Memory(const uint8_t* data, size_t size)
{
    drmp3_config config;
    drmp3_uint64 totalFrames;
    float* pcm = drmp3_open_memory_and_read_pcm_frames_f32(data, size, &config, &totalFrames, nullptr);
    if (!pcm || totalFrames == 0) return {};

    AudioFileData result;
    result.channels = static_cast<int>(config.channels);
    result.sampleRate = static_cast<int>(config.sampleRate);
    result.numFrames = static_cast<int>(totalFrames);
    result.samples.assign(pcm, pcm + totalFrames * config.channels);
    drmp3_free(pcm, nullptr);
    return result;
}

// ---------------------------------------------------------------------------
// OGG Opus (optional)
// ---------------------------------------------------------------------------

#if defined(BROAUDIO_HAS_OPUS) && BROAUDIO_HAS_OPUS

static AudioFileData loadOpus(const char* path)
{
    int error = 0;
    OggOpusFile* of = op_open_file(path, &error);
    if (!of) return {};

    int channels = op_channel_count(of, -1);
    ogg_int64_t totalSamples = op_pcm_total(of, -1);
    if (totalSamples <= 0 || channels <= 0) {
        op_free(of);
        return {};
    }

    AudioFileData result;
    result.channels = channels;
    result.sampleRate = 48000;  // Opus always decodes to 48 kHz
    result.numFrames = static_cast<int>(totalSamples);
    result.samples.resize(static_cast<size_t>(totalSamples) * channels);

    size_t offset = 0;
    size_t remaining = result.samples.size();
    while (remaining > 0) {
        int read = op_read_float(of, result.samples.data() + offset,
                                 static_cast<int>(std::min(remaining, static_cast<size_t>(8192))), nullptr);
        if (read <= 0) break;
        size_t got = static_cast<size_t>(read) * channels;
        offset += got;
        remaining -= got;
    }

    result.numFrames = static_cast<int>(offset / channels);
    result.samples.resize(offset);
    op_free(of);
    return result;
}

static AudioFileData loadOpusMemory(const uint8_t* data, size_t size)
{
    int error = 0;
    OggOpusFile* of = op_open_memory(data, size, &error);
    if (!of) return {};

    int channels = op_channel_count(of, -1);
    ogg_int64_t totalSamples = op_pcm_total(of, -1);
    if (totalSamples <= 0 || channels <= 0) {
        op_free(of);
        return {};
    }

    AudioFileData result;
    result.channels = channels;
    result.sampleRate = 48000;
    result.numFrames = static_cast<int>(totalSamples);
    result.samples.resize(static_cast<size_t>(totalSamples) * channels);

    size_t offset = 0;
    size_t remaining = result.samples.size();
    while (remaining > 0) {
        int read = op_read_float(of, result.samples.data() + offset,
                                 static_cast<int>(std::min(remaining, static_cast<size_t>(8192))), nullptr);
        if (read <= 0) break;
        size_t got = static_cast<size_t>(read) * channels;
        offset += got;
        remaining -= got;
    }

    result.numFrames = static_cast<int>(offset / channels);
    result.samples.resize(offset);
    op_free(of);
    return result;
}

#endif // BROAUDIO_HAS_OPUS

// ---------------------------------------------------------------------------
// Format detection from memory (magic bytes)
// ---------------------------------------------------------------------------

enum class AudioFormat { Unknown, Wav, Flac, Mp3, Opus };

static AudioFormat detectFormat(const uint8_t* data, size_t size)
{
    if (size < 4) return AudioFormat::Unknown;

    // RIFF....WAVE
    if (size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F'
        && data[8] == 'W' && data[9] == 'A' && data[10] == 'V' && data[11] == 'E')
        return AudioFormat::Wav;

    // fLaC
    if (data[0] == 'f' && data[1] == 'L' && data[2] == 'a' && data[3] == 'C')
        return AudioFormat::Flac;

    // OggS (OGG container — could be Opus or Vorbis)
    if (data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S')
        return AudioFormat::Opus;  // assume Opus if we have support

    // MP3: ID3 tag or sync word (0xFF 0xFB/0xFA/0xF3/0xF2/0xE3/0xE2)
    if (data[0] == 'I' && data[1] == 'D' && data[2] == '3')
        return AudioFormat::Mp3;
    if (size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0)
        return AudioFormat::Mp3;

    return AudioFormat::Unknown;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AudioFileData loadAudioFile(const char* path)
{
    if (!path) return {};

    std::string ext = extensionLower(path);

    if (ext == ".wav")
        return loadWav(path);
    if (ext == ".flac")
        return loadFlac(path);
    if (ext == ".mp3")
        return loadMp3(path);
    if (ext == ".ogg" || ext == ".opus") {
#if defined(BROAUDIO_HAS_OPUS) && BROAUDIO_HAS_OPUS
        return loadOpus(path);
#else
        return {};  // Opus support not compiled in
#endif
    }

    return {};
}

AudioFileData loadAudioFileFromMemory(const uint8_t* data, size_t size)
{
    if (!data || size == 0) return {};

    AudioFormat fmt = detectFormat(data, size);
    switch (fmt) {
        case AudioFormat::Wav:  return loadWavMemory(data, size);
        case AudioFormat::Flac: return loadFlacMemory(data, size);
        case AudioFormat::Mp3:  return loadMp3Memory(data, size);
        case AudioFormat::Opus:
#if defined(BROAUDIO_HAS_OPUS) && BROAUDIO_HAS_OPUS
            return loadOpusMemory(data, size);
#else
            return {};
#endif
        default: return {};
    }
}

bool saveWav(const char* path, const float* samples, int numFrames, int channels, int sampleRate)
{
    if (!path || !samples || numFrames <= 0 || channels < 1 || sampleRate <= 0)
        return false;

    drwav wav;
    drwav_data_format format;
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = static_cast<unsigned int>(channels);
    format.sampleRate = static_cast<unsigned int>(sampleRate);
    format.bitsPerSample = 32;

    if (!drwav_init_file_write(&wav, path, &format, nullptr))
        return false;

    drwav_uint64 written = drwav_write_pcm_frames(&wav, static_cast<drwav_uint64>(numFrames), samples);
    drwav_uninit(&wav);

    return written == static_cast<drwav_uint64>(numFrames);
}

} // namespace broaudio
