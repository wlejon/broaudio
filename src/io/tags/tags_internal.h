#pragma once

// Internal to the tag reader (src/io/tags/): byte and text helpers and the
// per-container readers that audio_tags.cpp dispatches to. Each reader
// returns false when the bytes are not its format.

#include "broaudio/io/audio_tags.h"
#include "../byte_source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace broaudio::tags {

// A tag block bigger than this (a huge embedded picture) is cut, not read whole.
constexpr size_t kMaxBlock = 32u * 1024u * 1024u;

inline uint32_t be16(const uint8_t* b) { return (uint32_t(b[0]) << 8) | b[1]; }
inline uint32_t be24(const uint8_t* b) { return (uint32_t(b[0]) << 16) | (uint32_t(b[1]) << 8) | b[2]; }
inline uint32_t be32(const uint8_t* b)
{
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}
inline uint64_t be64(const uint8_t* b) { return (uint64_t(be32(b)) << 32) | be32(b + 4); }
inline uint32_t le16(const uint8_t* b) { return b[0] | (uint32_t(b[1]) << 8); }
inline uint32_t le32(const uint8_t* b)
{
    return b[0] | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}
inline uint64_t le64(const uint8_t* b) { return le32(b) | (uint64_t(le32(b + 4)) << 32); }
// A 28-bit "syncsafe" integer: four bytes of seven bits each (ID3v2).
inline uint32_t syncsafe(const uint8_t* b)
{
    return (uint32_t(b[0] & 0x7f) << 21) | (uint32_t(b[1] & 0x7f) << 14) |
           (uint32_t(b[2] & 0x7f) << 7) | (b[3] & 0x7f);
}
inline bool isAscii(const uint8_t* b, size_t n, size_t at, const char* s)
{
    for (size_t i = 0; s[i]; ++i)
        if (at + i >= n || b[at + i] != static_cast<uint8_t>(s[i])) return false;
    return true;
}

// Text in the encodings tags use, to UTF-8.
std::string latin1ToUtf8(const uint8_t* b, size_t n);
// UTF-16 with an optional byte-order mark; `bigEndian` when there is none.
std::string utf16ToUtf8(const uint8_t* b, size_t n, bool bigEndian);
// UTF-8 as stored (a leading BOM dropped, invalid sequences as U+FFFD).
std::string utf8Clean(const uint8_t* b, size_t n);
bool isValidUtf8(const uint8_t* b, size_t n);
std::string trim(const std::string& s);

// "3/12" -> 3, 12; "" -> 0, 0.
void numberPair(const std::string& s, int& n, int& of);
// The first four-digit run ("2021-05-03" -> 2021), else 0.
int yearOf(const std::string& s);
// The ID3v1 genre name for an index, or "" past the table.
std::string id3v1Genre(int index);
// "(17)", "17", "(17)Rock" -> "Rock"; anything else as given.
std::string genreName(const std::string& s);
std::vector<uint8_t> base64Decode(const uint8_t* b, size_t n);

// Keep the front cover (type 3); else the first picture offered.
void offerPicture(AudioTags& t, AudioTags::Picture&& pic);
// Fill what `t` left empty from `from` (strings, numbers, the picture).
void fillEmpty(AudioTags& t, const AudioTags& from);

// --- ID3 (id3.cpp) ---
// A v2 tag's whole length (header, body, footer) from its first 10 bytes, or 0.
uint64_t id3v2Size(const uint8_t* b, size_t n);
// Read the ID3v2 tag at `offset` into `t`; returns its length, 0 when there is none.
uint64_t readId3v2(ByteSource& src, uint64_t offset, AudioTags& t);
// Read the 128-byte ID3v1 tag at the end into `t`; false when there is none.
bool readId3v1(ByteSource& src, AudioTags& t);

// --- Vorbis comments and the FLAC picture block (vorbis.cpp) ---
// The comment block (little-endian lengths: vendor, count, "KEY=value"...).
void readVorbisComments(const uint8_t* b, size_t n, AudioTags& t);
// A METADATA_BLOCK_PICTURE body (big-endian), false when cut short.
bool readPictureBlock(const uint8_t* b, size_t n, AudioTags::Picture& out);

// --- the containers ---
bool readMp3(ByteSource& src, AudioTags& t);   // mp3.cpp
bool readFlac(ByteSource& src, AudioTags& t);  // vorbis.cpp
bool readOgg(ByteSource& src, AudioTags& t);   // ogg.cpp
bool readWav(ByteSource& src, AudioTags& t);   // wav.cpp
bool readMp4(ByteSource& src, AudioTags& t);   // mp4.cpp

// What the MP4 demuxer needs beyond minimp4's sample tables: the first sound
// track's timing (edit list, media timescale) and its codec. mp4.cpp.
struct Mp4AudioInfo {
    bool found = false;
    uint32_t format = 0;            // sample entry fourcc: 'mp4a', 'alac', ...
    uint8_t objectType = 0;         // esds objectTypeIndication (0x40 = MPEG-4 audio)
    int channels = 0;
    int sampleRate = 0;             // from the sample entry (0 when it does not fit 16.16)
    uint32_t mediaTimescale = 0;    // mdhd
    uint64_t mediaDuration = 0;
    uint32_t movieTimescale = 0;    // mvhd
    uint64_t movieDuration = 0;
    int64_t editMediaTime = -1;     // the first non-empty edit's media_time (media timescale)
    uint64_t editDuration = 0;      // its segment_duration (movie timescale)
    uint32_t avgBitrate = 0;        // esds
    uint64_t mdatBytes = 0;
    std::vector<uint8_t> asc;       // AudioSpecificConfig
};
// Walk the boxes; `tags` (optional) gets the ilst items. False when not MP4.
bool readMp4Info(ByteSource& src, Mp4AudioInfo& info, AudioTags* tags);

} // namespace broaudio::tags
