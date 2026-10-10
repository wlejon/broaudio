// Vorbis comments ("TITLE=...", "ARTIST=...") as FLAC, Ogg Vorbis and Ogg
// Opus carry them; the FLAC picture block (in FLAC directly, in Ogg as a
// base64 METADATA_BLOCK_PICTURE comment); and FLAC's metadata blocks.

#include "tags_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace broaudio::tags {

namespace {

enum class Key { None, Title, Artist, Album, AlbumArtist, Track, TrackTotal, Disc, DiscTotal, Year, Genre, Picture };

Key keyOf(const std::string& upper)
{
    static const struct { const char* k; Key v; } kKeys[] = {
        {"TITLE", Key::Title}, {"ARTIST", Key::Artist}, {"ALBUM", Key::Album},
        {"ALBUMARTIST", Key::AlbumArtist}, {"ALBUM ARTIST", Key::AlbumArtist},
        {"ALBUM_ARTIST", Key::AlbumArtist}, {"TRACKNUMBER", Key::Track},
        {"TRACKTOTAL", Key::TrackTotal}, {"TOTALTRACKS", Key::TrackTotal},
        {"DISCNUMBER", Key::Disc}, {"DISCTOTAL", Key::DiscTotal}, {"TOTALDISCS", Key::DiscTotal},
        {"DATE", Key::Year}, {"YEAR", Key::Year}, {"GENRE", Key::Genre},
        {"METADATA_BLOCK_PICTURE", Key::Picture},
    };
    for (const auto& e : kKeys)
        if (upper == e.k) return e.v;
    return Key::None;
}

} // namespace

bool readPictureBlock(const uint8_t* b, size_t n, AudioTags::Picture& out)
{
    if (n < 32) return false;
    size_t o = 0;
    const uint32_t type = be32(b + o); o += 4;
    const uint32_t mimeLen = be32(b + o); o += 4;
    if (mimeLen > n - o) return false;
    std::string mime(reinterpret_cast<const char*>(b + o), mimeLen);
    o += mimeLen;
    for (auto& c : mime) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (mime.empty()) mime = "image/jpeg";
    if (o + 4 > n) return false;
    const uint32_t descLen = be32(b + o); o += 4;
    if (descLen > n - o) return false;
    o += descLen;
    o += 16;                                                      // width, height, depth, colours
    if (o + 4 > n) return false;
    const uint32_t len = be32(b + o); o += 4;
    if (!len || len > n - o) return false;
    out.mime = std::move(mime);
    out.type = static_cast<int>(type);
    out.bytes.assign(b + o, b + o + len);
    return true;
}

void readVorbisComments(const uint8_t* b, size_t n, AudioTags& t)
{
    size_t o = 0;
    if (o + 8 > n) return;
    const uint32_t vendor = le32(b + o);
    o += 4;
    if (vendor > n - o) return;
    o += vendor;
    if (o + 4 > n) return;
    const uint32_t count = le32(b + o);
    o += 4;
    std::vector<std::string> artists;
    for (uint32_t i = 0; i < count && o + 4 <= n; ++i) {
        const uint32_t len = le32(b + o);
        o += 4;
        if (len > n - o) break;
        const uint8_t* e = b + o;
        o += len;
        const uint8_t* eq = static_cast<const uint8_t*>(std::memchr(e, '=', len));
        if (!eq || eq == e) continue;
        std::string key(reinterpret_cast<const char*>(e), static_cast<size_t>(eq - e));
        for (auto& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const Key k = keyOf(key);
        if (k == Key::None) continue;
        const uint8_t* v = eq + 1;
        const size_t vn = len - static_cast<size_t>(v - e);
        if (k == Key::Picture) {
            const std::vector<uint8_t> raw = base64Decode(v, vn);
            AudioTags::Picture pic;
            if (readPictureBlock(raw.data(), raw.size(), pic)) offerPicture(t, std::move(pic));
            continue;
        }
        std::string value = trim(utf8Clean(v, vn));
        if (value.empty()) continue;
        int num = 0, of = 0;
        switch (k) {
            case Key::Artist: artists.push_back(std::move(value)); break;
            case Key::Track:
                numberPair(value, num, of);
                t.track = num;
                if (of) t.trackTotal = of;
                break;
            case Key::Disc:
                numberPair(value, num, of);
                t.disc = num;
                if (of) t.discTotal = of;
                break;
            case Key::TrackTotal: numberPair(value, num, of); t.trackTotal = num; break;
            case Key::DiscTotal: numberPair(value, num, of); t.discTotal = num; break;
            case Key::Year: if (!t.year) t.year = yearOf(value); break;
            case Key::Title: if (t.title.empty()) t.title = value; break;
            case Key::Album: if (t.album.empty()) t.album = value; break;
            case Key::AlbumArtist: if (t.albumArtist.empty()) t.albumArtist = value; break;
            case Key::Genre: if (t.genre.empty()) t.genre = value; break;
            default: break;
        }
    }
    if (!artists.empty()) {
        std::string joined;
        for (const auto& a : artists) {
            if (!joined.empty()) joined += ", ";
            joined += a;
        }
        t.artist = joined;
    }
}

// FLAC: STREAMINFO gives the length, VORBIS_COMMENT the tags, PICTURE the
// cover. An ID3v2 tag in front of "fLaC" (some taggers write one) is read
// past, and fills what the comments leave empty.
bool readFlac(ByteSource& src, AudioTags& t)
{
    AudioTags id3;
    uint64_t o = readId3v2(src, 0, id3);
    const std::vector<uint8_t> magic = src.read(o, 4);
    if (!isAscii(magic.data(), magic.size(), 0, "fLaC")) return false;
    o += 4;
    t.container = "flac";
    t.codec = "flac";
    uint64_t total = 0;
    for (int guard = 0; guard < 1024 && o + 4 <= src.size(); ++guard) {
        uint8_t head[4];
        if (src.read(o, head, 4) < 4) break;
        const bool last = (head[0] & 0x80) != 0;
        const int type = head[0] & 0x7f;
        const uint32_t len = be24(head + 1);
        o += 4;
        if (type == 0 && len >= 18) {
            const std::vector<uint8_t> b = src.read(o, 18);
            if (b.size() == 18) {
                t.sampleRate = static_cast<int>((uint32_t(b[10]) << 12) | (uint32_t(b[11]) << 4) | (b[12] >> 4));
                t.channels = ((b[12] >> 1) & 7) + 1;
                // 36 bits of total samples: the low 4 bits of byte 13, then 4 bytes.
                total = (uint64_t(b[13] & 0x0f) << 32) | be32(b.data() + 14);
            }
        } else if (type == 4 && len <= kMaxBlock) {
            const std::vector<uint8_t> b = src.read(o, len);
            readVorbisComments(b.data(), b.size(), t);
        } else if (type == 6 && len <= kMaxBlock) {
            const std::vector<uint8_t> b = src.read(o, len);
            AudioTags::Picture pic;
            if (readPictureBlock(b.data(), b.size(), pic)) offerPicture(t, std::move(pic));
        }
        o += len;
        if (last) break;
    }
    fillEmpty(t, id3);
    if (t.sampleRate && total) {
        t.duration = double(total) / t.sampleRate;
        const uint64_t audio = src.size() > o ? src.size() - o : 0;
        t.bitrate = static_cast<int>(double(audio) * 8 / t.duration + 0.5);
    }
    return true;
}

} // namespace broaudio::tags
