#include "broaudio/io/audio_stream.h"

#include <cstdio>
#include <cstring>

// Declarations only — the dr_* implementations are compiled in audio_file.cpp
// and stb_vorbis in src/codec/stb_vorbis_impl.c.
#include "dr_wav.h"
#include "dr_flac.h"
#include "dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

namespace broaudio {

// ---------------------------------------------------------------------------
// Backend state
// ---------------------------------------------------------------------------

struct AudioFileStream::Impl {
    enum class Backend { None, Wav, Flac, Mp3, Vorbis };
    Backend backend = Backend::None;

    drwav wav{};
    bool wavOpen = false;
    drflac* flac = nullptr;
    drmp3 mp3{};
    bool mp3Open = false;
    stb_vorbis* vorbis = nullptr;

    ~Impl() { close(); }

    void close()
    {
        switch (backend) {
            case Backend::Wav:  if (wavOpen) drwav_uninit(&wav); wavOpen = false; break;
            case Backend::Flac: if (flac) drflac_close(flac); flac = nullptr; break;
            case Backend::Mp3:  if (mp3Open) drmp3_uninit(&mp3); mp3Open = false; break;
            case Backend::Vorbis: if (vorbis) stb_vorbis_close(vorbis); vorbis = nullptr; break;
            default: break;
        }
        backend = Backend::None;
    }
};

// ---------------------------------------------------------------------------
// Format sniff (mirror of audio_file.cpp's detectFormat, kept local so the
// public header stays free of internal enums)
// ---------------------------------------------------------------------------

namespace {
enum class Sniffed { Unknown, Wav, Flac, Mp3, OggVorbis, OggOpus, OggOther };

Sniffed sniff(const uint8_t* d, size_t n)
{
    if (n < 4) return Sniffed::Unknown;
    if (n >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F'
        && d[8] == 'W' && d[9] == 'A' && d[10] == 'V' && d[11] == 'E')
        return Sniffed::Wav;
    if (d[0] == 'f' && d[1] == 'L' && d[2] == 'a' && d[3] == 'C')
        return Sniffed::Flac;
    if (d[0] == 'O' && d[1] == 'g' && d[2] == 'g' && d[3] == 'S') {
        if (n < 28) return Sniffed::OggOther;
        const size_t packetStart = 27 + d[26];
        if (n < packetStart + 8) return Sniffed::OggOther;
        const uint8_t* p = d + packetStart;
        if (std::memcmp(p, "OpusHead", 8) == 0) return Sniffed::OggOpus;
        if (p[0] == 0x01 && std::memcmp(p + 1, "vorbis", 6) == 0) return Sniffed::OggVorbis;
        return Sniffed::OggOther;
    }
    if (d[0] == 'I' && d[1] == 'D' && d[2] == '3')
        return Sniffed::Mp3;
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0)
        return Sniffed::Mp3;
    return Sniffed::Unknown;
}
} // namespace

// ---------------------------------------------------------------------------
// AudioFileStream
// ---------------------------------------------------------------------------

AudioFileStream::AudioFileStream() : impl_(new Impl) {}
AudioFileStream::~AudioFileStream() = default;

bool AudioFileStream::isOpen() const { return impl_->backend != Impl::Backend::None; }

void AudioFileStream::close()
{
    impl_->close();
    channels_ = 0;
    sampleRate_ = 0;
    totalFrames_ = 0;
    error_.clear();
}

bool AudioFileStream::open(const char* path)
{
    close();
    if (!path || !*path) {
        error_ = "no file path given";
        return false;
    }

    uint8_t head[512];
    size_t got = 0;
    if (FILE* f = fopen(path, "rb")) {
        got = fread(head, 1, sizeof(head), f);
        fclose(f);
    } else {
        error_ = std::string("cannot open file: ") + path;
        return false;
    }

    switch (sniff(head, got)) {
        case Sniffed::Wav: {
            if (!drwav_init_file(&impl_->wav, path, nullptr)) {
                error_ = "corrupt or unsupported WAV file";
                return false;
            }
            impl_->wavOpen = true;
            impl_->backend = Impl::Backend::Wav;
            channels_ = static_cast<int>(impl_->wav.channels);
            sampleRate_ = static_cast<int>(impl_->wav.sampleRate);
            totalFrames_ = impl_->wav.totalPCMFrameCount;
            return true;
        }
        case Sniffed::Flac: {
            impl_->flac = drflac_open_file(path, nullptr);
            if (!impl_->flac) {
                error_ = "corrupt or unsupported FLAC file";
                return false;
            }
            impl_->backend = Impl::Backend::Flac;
            channels_ = static_cast<int>(impl_->flac->channels);
            sampleRate_ = static_cast<int>(impl_->flac->sampleRate);
            totalFrames_ = impl_->flac->totalPCMFrameCount;
            return true;
        }
        case Sniffed::Mp3: {
            if (!drmp3_init_file(&impl_->mp3, path, nullptr)) {
                error_ = "corrupt or unsupported MP3 file";
                return false;
            }
            impl_->mp3Open = true;
            impl_->backend = Impl::Backend::Mp3;
            channels_ = static_cast<int>(impl_->mp3.channels);
            sampleRate_ = static_cast<int>(impl_->mp3.sampleRate);
            totalFrames_ = 0; // counting would scan the whole file
            return true;
        }
        case Sniffed::OggVorbis: {
            int err = VORBIS__no_error;
            impl_->vorbis = stb_vorbis_open_filename(path, &err, nullptr);
            if (!impl_->vorbis) {
                error_ = "corrupt Ogg Vorbis file (stb_vorbis error "
                         + std::to_string(err) + ")";
                return false;
            }
            impl_->backend = Impl::Backend::Vorbis;
            stb_vorbis_info info = stb_vorbis_get_info(impl_->vorbis);
            channels_ = info.channels;
            sampleRate_ = static_cast<int>(info.sample_rate);
            totalFrames_ = stb_vorbis_stream_length_in_samples(impl_->vorbis);
            return true;
        }
        case Sniffed::OggOpus:
            error_ = "Ogg Opus cannot be disk-streamed; re-encode as WAV, FLAC, "
                     "MP3, or Ogg Vorbis";
            return false;
        case Sniffed::OggOther:
            error_ = "unrecognized Ogg codec (streamable formats: WAV, FLAC, MP3, "
                     "Ogg Vorbis)";
            return false;
        default:
            error_ = "unrecognized audio format (streamable formats: WAV, FLAC, "
                     "MP3, Ogg Vorbis)";
            return false;
    }
}

int AudioFileStream::readFrames(float* dst, int maxFrames)
{
    if (!dst || maxFrames <= 0) return 0;
    switch (impl_->backend) {
        case Impl::Backend::Wav:
            return static_cast<int>(drwav_read_pcm_frames_f32(
                &impl_->wav, static_cast<drwav_uint64>(maxFrames), dst));
        case Impl::Backend::Flac:
            return static_cast<int>(drflac_read_pcm_frames_f32(
                impl_->flac, static_cast<drflac_uint64>(maxFrames), dst));
        case Impl::Backend::Mp3:
            return static_cast<int>(drmp3_read_pcm_frames_f32(
                &impl_->mp3, static_cast<drmp3_uint64>(maxFrames), dst));
        case Impl::Backend::Vorbis:
            return stb_vorbis_get_samples_float_interleaved(
                impl_->vorbis, channels_, dst, maxFrames * channels_);
        default:
            return 0;
    }
}

bool AudioFileStream::seekToStart()
{
    // stb_vorbis has a dedicated (cheaper) rewind; the dr_* backends just
    // seek to frame 0.
    if (impl_->backend == Impl::Backend::Vorbis)
        return stb_vorbis_seek_start(impl_->vorbis) != 0;
    return seekToFrame(0);
}

bool AudioFileStream::seekToFrame(uint64_t frame)
{
    switch (impl_->backend) {
        case Impl::Backend::Wav:
            return drwav_seek_to_pcm_frame(&impl_->wav,
                       static_cast<drwav_uint64>(frame)) == DRWAV_TRUE;
        case Impl::Backend::Flac:
            return drflac_seek_to_pcm_frame(impl_->flac,
                       static_cast<drflac_uint64>(frame)) == DRFLAC_TRUE;
        case Impl::Backend::Mp3:
            return drmp3_seek_to_pcm_frame(&impl_->mp3,
                       static_cast<drmp3_uint64>(frame)) == DRMP3_TRUE;
        case Impl::Backend::Vorbis:
            return stb_vorbis_seek(impl_->vorbis,
                       static_cast<unsigned int>(frame)) != 0;
        default:
            return false;
    }
}

} // namespace broaudio
