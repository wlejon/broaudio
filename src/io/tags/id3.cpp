// ID3v2.2, v2.3 and v2.4 (at the start of an MP3, and now and then in front
// of a FLAC or inside a WAV "id3 " chunk) and the 128-byte ID3v1 at the end.
//
// Text frames are read in every encoding ID3 allows: ISO-8859-1, UTF-16 with
// a byte-order mark, UTF-16BE and UTF-8. A frame with several values (v2.4
// separates them with a zero) keeps the first, except the artist, whose
// values are joined with ", ". Unsynchronisation (whole-tag in v2.2/2.3, per
// frame or whole-tag in v2.4) is undone; compressed and encrypted frames are
// skipped.

#include "tags_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace broaudio::tags {

uint64_t id3v2Size(const uint8_t* b, size_t n)
{
    if (n < 10 || !isAscii(b, n, 0, "ID3")) return 0;
    if (b[3] < 2 || b[3] > 4) return 0;
    if ((b[6] | b[7] | b[8] | b[9]) & 0x80) return 0;        // not syncsafe: not a tag
    return 10 + uint64_t(syncsafe(b + 6)) + ((b[5] & 0x10) ? 10 : 0);
}

namespace {

// Undo unsynchronisation: every FF 00 back to FF.
std::vector<uint8_t> unsync(const uint8_t* b, size_t n)
{
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(b[i]);
        if (b[i] == 0xff && i + 1 < n && b[i + 1] == 0) ++i;
    }
    return out;
}

// The end of a zero-terminated string from `o`: one zero byte, or two
// aligned ones for UTF-16; n when there is none.
size_t terminator(const uint8_t* b, size_t n, size_t o, bool wide)
{
    if (!wide) {
        for (size_t i = o; i < n; ++i)
            if (b[i] == 0) return i;
        return n;
    }
    for (size_t i = o; i + 1 < n; i += 2)
        if (b[i] == 0 && b[i + 1] == 0) return i;
    return n;
}

std::string decode(uint8_t enc, const uint8_t* b, size_t n)
{
    switch (enc) {
        case 1: return utf16ToUtf8(b, n, false);
        case 2: return utf16ToUtf8(b, n, true);
        case 3: return utf8Clean(b, n);
        default: return latin1ToUtf8(b, n);
    }
}

// A text frame's values (zero-separated in v2.4), empty ones dropped.
std::vector<std::string> textValues(const uint8_t* d, size_t n)
{
    std::vector<std::string> out;
    if (!n) return out;
    const uint8_t enc = d[0];
    const bool wide = enc == 1 || enc == 2;
    // A UTF-16 value without its own byte-order mark follows the first's.
    const bool firstBE = n >= 3 && d[1] == 0xfe && d[2] == 0xff;
    size_t o = 1;
    while (o < n) {
        const size_t end = terminator(d, n, o, wide);
        std::string s;
        if (enc == 1 && !out.empty() && !(d[o] == 0xff || d[o] == 0xfe))
            s = utf16ToUtf8(d + o, end - o, firstBE);
        else
            s = decode(enc, d + o, end - o);
        s = trim(s);
        if (!s.empty()) out.push_back(std::move(s));
        o = end + (wide ? 2 : 1);
    }
    return out;
}

// An attached picture: APIC (v2.3+) or PIC (v2.2).
bool picture(const uint8_t* d, size_t n, bool v22, AudioTags::Picture& out)
{
    if (n < 4) return false;
    const uint8_t enc = d[0];
    size_t o;
    std::string mime;
    if (v22) {
        std::string fmt(reinterpret_cast<const char*>(d + 1), 3);
        for (auto& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        mime = fmt == "png" ? "image/png" : "image/jpeg";
        o = 4;
    } else {
        const size_t end = terminator(d, n, 1, false);
        mime = std::string(reinterpret_cast<const char*>(d + 1), end - 1);
        for (auto& c : mime) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (mime.empty()) mime = "image/jpeg";
        if (mime.find('/') == std::string::npos) mime = "image/" + (mime == "jpg" ? std::string("jpeg") : mime);
        o = end + 1;
    }
    if (o >= n) return false;
    const int type = d[o++];
    const bool wide = enc == 1 || enc == 2;
    o = terminator(d, n, o, wide) + (wide ? 2 : 1);           // the description
    if (o >= n) return false;
    out.mime = std::move(mime);
    out.type = type;
    out.bytes.assign(d + o, d + n);
    return true;
}

enum class Field { None, Title, Artist, Album, AlbumArtist, Track, Disc, Year, Genre };

Field fieldOf(const std::string& id)
{
    static const struct { const char* id; Field f; } kIds[] = {
        {"TIT2", Field::Title}, {"TPE1", Field::Artist}, {"TALB", Field::Album},
        {"TPE2", Field::AlbumArtist}, {"TRCK", Field::Track}, {"TPOS", Field::Disc},
        {"TYER", Field::Year}, {"TDRC", Field::Year}, {"TCON", Field::Genre},
        {"TT2", Field::Title}, {"TP1", Field::Artist}, {"TAL", Field::Album},
        {"TP2", Field::AlbumArtist}, {"TRK", Field::Track}, {"TPA", Field::Disc},
        {"TYE", Field::Year}, {"TCO", Field::Genre},
    };
    for (const auto& e : kIds)
        if (id == e.id) return e.f;
    return Field::None;
}

void setField(AudioTags& t, Field f, const std::vector<std::string>& values)
{
    if (values.empty()) return;
    const std::string& v = values[0];
    switch (f) {
        case Field::Title: t.title = v; break;
        case Field::Album: t.album = v; break;
        case Field::AlbumArtist: t.albumArtist = v; break;
        case Field::Artist: {
            std::string joined;
            for (const auto& s : values) {
                if (!joined.empty()) joined += ", ";
                joined += s;
            }
            t.artist = joined;
            break;
        }
        case Field::Track: numberPair(v, t.track, t.trackTotal); break;
        case Field::Disc: numberPair(v, t.disc, t.discTotal); break;
        case Field::Year: if (!t.year) t.year = yearOf(v); break;
        case Field::Genre: t.genre = genreName(v); break;
        default: break;
    }
}

} // namespace

uint64_t readId3v2(ByteSource& src, uint64_t offset, AudioTags& t)
{
    const std::vector<uint8_t> head = src.read(offset, 10);
    const uint64_t size = id3v2Size(head.data(), head.size());
    if (!size) return 0;
    const int ver = head[3];
    const uint8_t flags = head[5];
    std::vector<uint8_t> body = src.read(offset + 10, static_cast<size_t>(std::min<uint64_t>(size - 10, kMaxBlock)));
    if (ver < 4 && (flags & 0x80)) body = unsync(body.data(), body.size());
    const bool v22 = ver == 2;
    if (v22 && (flags & 0x40)) return size;                 // v2.2 "compressed": no scheme was ever defined

    size_t o = 0;
    if (!v22 && (flags & 0x40) && body.size() >= 4) {
        // Extended header: v2.3 counts the bytes after its size field, v2.4 its whole length.
        o = ver == 4 ? syncsafe(body.data()) : be32(body.data()) + 4;
    }
    const size_t idLen = v22 ? 3 : 4;
    const size_t headLen = v22 ? 6 : 10;
    const size_t n = body.size();
    while (o + headLen <= n) {
        const uint8_t* fh = body.data() + o;
        if (fh[0] == 0) break;                                // padding
        std::string id(reinterpret_cast<const char*>(fh), idLen);
        bool okId = true;
        for (char c : id)
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) { okId = false; break; }
        if (!okId) break;
        uint64_t len = v22 ? be24(fh + 3) : ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        const uint32_t fflags = v22 ? 0 : be16(fh + 8);
        o += headLen;
        if (len == 0 || o + len > n) {
            // Some v2.4 writers store plain sizes; try that before giving up.
            if (ver == 4) len = be32(fh + 4);
            if (len == 0 || o + len > n) break;
        }
        const uint8_t* data = body.data() + o;
        size_t dlen = static_cast<size_t>(len);
        o += static_cast<size_t>(len);

        std::vector<uint8_t> undone;
        if (ver == 3) {
            if (fflags & 0x00c0) continue;                    // compressed / encrypted
            if (fflags & 0x0020) { if (dlen < 1) continue; data += 1; dlen -= 1; }   // group id
        } else if (ver == 4) {
            if (fflags & 0x000c) continue;                    // compressed / encrypted
            if (fflags & 0x0040) { if (dlen < 1) continue; data += 1; dlen -= 1; }   // group id
            if (fflags & 0x0001) { if (dlen < 4) continue; data += 4; dlen -= 4; }   // data length indicator
            if ((fflags & 0x0002) || (flags & 0x80)) {
                undone = unsync(data, dlen);
                data = undone.data();
                dlen = undone.size();
            }
        }
        if (id == "APIC" || id == "PIC") {
            AudioTags::Picture pic;
            if (picture(data, dlen, v22, pic)) offerPicture(t, std::move(pic));
            continue;
        }
        const Field f = fieldOf(id);
        if (f != Field::None) setField(t, f, textValues(data, dlen));
    }
    return size;
}

bool readId3v1(ByteSource& src, AudioTags& t)
{
    if (src.size() < 128) return false;
    const std::vector<uint8_t> b = src.read(src.size() - 128, 128);
    if (b.size() < 128 || !isAscii(b.data(), b.size(), 0, "TAG")) return false;
    auto field = [&](size_t o, size_t n) {
        size_t end = o;
        while (end < o + n && b[end] != 0) ++end;
        return trim(latin1ToUtf8(b.data() + o, end - o));
    };
    t.title = field(3, 30);
    t.artist = field(33, 30);
    t.album = field(63, 30);
    t.year = yearOf(field(93, 4));
    if (b[125] == 0 && b[126] != 0) t.track = b[126];         // ID3v1.1
    t.genre = id3v1Genre(b[127]);
    return true;
}

} // namespace broaudio::tags
