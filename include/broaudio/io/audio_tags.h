#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace broaudio {

// A music file's tags, length and format, read without decoding audio and
// without reading the whole file: only the headers, the tag blocks, and (for
// a length) the last Ogg page or an MP3 frame header or two.
//
//   Container  Tags                                      Length
//   MP3        ID3v2.2/2.3/2.4 (unsynchronisation,       Xing/Info or VBRI frame count;
//              extended headers, UTF-16), ID3v1 fills    else the bit rate (CBR), else
//              what v2 left empty                        a scan of frame headers (VBR)
//   FLAC       VORBIS_COMMENT, PICTURE (ID3v2 in front   STREAMINFO
//              is read past)
//   Ogg        Vorbis / Opus comments, incl. a base64    last page's granule (less the
//              METADATA_BLOCK_PICTURE; a comment packet  Opus pre-skip)
//              over many pages is put back together
//   WAV        LIST/INFO, an "id3 " chunk                data size over byte rate
//   MP4/M4A    moov/udta/meta/ilst (©nam ©ART ©alb aART  edit list, else mdhd, else
//              trkn disk ©day ©gen gnre covr)            mvhd
//
// Strings are UTF-8, empty when absent; numbers are 0 when absent. Several
// artists (repeated Vorbis ARTIST, ID3v2.4 zero-separated values) are joined
// with ", ".
struct AudioTags {
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    std::string genre;
    int track = 0;
    int trackTotal = 0;
    int disc = 0;
    int discTotal = 0;
    int year = 0;

    double duration = 0.0;  // seconds; 0 when the file does not say
    int sampleRate = 0;
    int channels = 0;
    int bitrate = 0;        // average bits per second of the audio; 0 when unknown
    std::string codec;      // "mp3", "flac", "vorbis", "opus", "pcm", "float", "aac", "alac", ...
    std::string container;  // "mp3", "flac", "ogg", "wav", "mp4"

    // The front cover (picture type 3), else the first picture.
    struct Picture {
        std::string mime;   // "image/jpeg", "image/png", ...
        int type = -1;      // ID3/FLAC picture type; -1 when the format has none (MP4)
        std::vector<uint8_t> bytes;
    };
    bool hasPicture = false;
    Picture picture;
};

// Read a file's tags. Returns false (with *error set) when the file cannot
// be opened or is not a format listed above; a recognised file with no tags
// returns true with empty fields. The format is found from the bytes, not
// the extension.
bool readAudioTags(const char* path, AudioTags& out, std::string* error = nullptr);

// The same over bytes in memory.
bool readAudioTagsFromMemory(const uint8_t* data, size_t size, AudioTags& out,
                             std::string* error = nullptr);

} // namespace broaudio
