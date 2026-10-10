// The tag reader's entry points, the format sniff, and the text helpers the
// per-container readers share.

#include "tags_internal.h"

#include <cctype>
#include <cstring>

namespace broaudio {
namespace tags {

static void putUtf8(std::string& s, uint32_t cp)
{
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xc0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xe0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        s += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        s += static_cast<char>(0xf0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        s += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

std::string latin1ToUtf8(const uint8_t* b, size_t n)
{
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) putUtf8(s, b[i]);
    return s;
}

std::string utf16ToUtf8(const uint8_t* b, size_t n, bool bigEndian)
{
    size_t i = 0;
    bool be = bigEndian;
    if (n >= 2 && b[0] == 0xff && b[1] == 0xfe) { be = false; i = 2; }
    else if (n >= 2 && b[0] == 0xfe && b[1] == 0xff) { be = true; i = 2; }
    std::string s;
    s.reserve(n);
    auto unit = [&](size_t at) -> uint32_t {
        return be ? (uint32_t(b[at]) << 8) | b[at + 1] : b[at] | (uint32_t(b[at + 1]) << 8);
    };
    for (; i + 1 < n; i += 2) {
        uint32_t c = unit(i);
        if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
            const uint32_t lo = unit(i + 2);
            if (lo >= 0xdc00 && lo < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i += 2;
            } else {
                c = 0xfffd;
            }
        } else if (c >= 0xd800 && c < 0xe000) {
            c = 0xfffd;
        }
        putUtf8(s, c);
    }
    return s;
}

// The length of the valid UTF-8 sequence at b[i], or 0.
static size_t utf8Seq(const uint8_t* b, size_t n, size_t i)
{
    const uint8_t c = b[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || i + len > n) return 0;
    for (size_t k = 1; k < len; ++k)
        if ((b[i + k] & 0xc0) != 0x80) return 0;
    if (len == 2 && c < 0xc2) return 0;                                     // overlong
    if (len == 3 && c == 0xe0 && b[i + 1] < 0xa0) return 0;
    if (len == 4 && (c > 0xf4 || (c == 0xf0 && b[i + 1] < 0x90))) return 0;
    return len;
}

bool isValidUtf8(const uint8_t* b, size_t n)
{
    for (size_t i = 0; i < n;) {
        const size_t len = utf8Seq(b, n, i);
        if (!len) return false;
        i += len;
    }
    return true;
}

std::string utf8Clean(const uint8_t* b, size_t n)
{
    size_t i = 0;
    if (n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) i = 3;
    std::string s;
    s.reserve(n);
    while (i < n) {
        const size_t len = utf8Seq(b, n, i);
        if (!len) { putUtf8(s, 0xfffd); ++i; continue; }
        s.append(reinterpret_cast<const char*>(b + i), len);
        i += len;
    }
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = 0, e = s.size();
    auto space = [](unsigned char c) { return c == 0 || std::isspace(c); };
    while (a < e && space(static_cast<unsigned char>(s[a]))) ++a;
    while (e > a && space(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(a, e - a);
}

void numberPair(const std::string& s, int& n, int& of)
{
    n = 0;
    of = 0;
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return;
    long v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        if (v < 100000000) v = v * 10 + (s[i] - '0');
        ++i;
    }
    n = static_cast<int>(v);
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size() || s[i] != '/') return;
    ++i;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    long w = 0;
    bool any = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        if (w < 100000000) w = w * 10 + (s[i] - '0');
        any = true;
        ++i;
    }
    if (any) of = static_cast<int>(w);
}

int yearOf(const std::string& s)
{
    for (size_t i = 0; i + 4 <= s.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < 4; ++k)
            if (!std::isdigit(static_cast<unsigned char>(s[i + k]))) { ok = false; break; }
        if (ok) return std::stoi(s.substr(i, 4));
    }
    return 0;
}

static const char* const kGenres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz",
    "Metal", "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno",
    "Industrial", "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno",
    "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
    "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock", "Bass", "Soul",
    "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes", "Trailer",
    "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
    "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin",
    "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock",
    "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening",
    "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
    "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
    "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock",
    "Drum Solo", "A Cappella", "Euro-House", "Dance Hall",
};

std::string id3v1Genre(int index)
{
    if (index < 0 || index >= static_cast<int>(sizeof(kGenres) / sizeof(kGenres[0]))) return {};
    return kGenres[index];
}

std::string genreName(const std::string& in)
{
    std::string s = trim(in);
    auto allDigits = [](const std::string& d) {
        if (d.empty() || d.size() > 3) return false;
        for (char c : d)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return true;
    };
    if (allDigits(s)) return id3v1Genre(std::stoi(s));
    if (s.size() >= 3 && s[0] == '(') {
        const size_t close = s.find(')');
        if (close != std::string::npos) {
            const std::string num = s.substr(1, close - 1);
            std::string rest = trim(s.substr(close + 1));
            if (num == "RX" && rest.empty()) return "Remix";
            if (num == "CR" && rest.empty()) return "Cover";
            if (allDigits(num)) return rest.empty() ? id3v1Genre(std::stoi(num)) : rest;
        }
    }
    return s;
}

std::vector<uint8_t> base64Decode(const uint8_t* b, size_t n)
{
    std::vector<uint8_t> out;
    out.reserve(n * 3 / 4 + 3);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = b[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;     // and the URL-safe spellings
        else if (c == '/' || c == '_') v = 63;
        else continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

void offerPicture(AudioTags& t, AudioTags::Picture&& pic)
{
    if (pic.bytes.empty()) return;
    if (!t.hasPicture || (t.picture.type != 3 && pic.type == 3)) {
        t.picture = std::move(pic);
        t.hasPicture = true;
    }
}

void fillEmpty(AudioTags& t, const AudioTags& from)
{
    auto str = [](std::string& a, const std::string& b) { if (a.empty()) a = b; };
    auto num = [](int& a, int b) { if (!a) a = b; };
    str(t.title, from.title);
    str(t.artist, from.artist);
    str(t.album, from.album);
    str(t.albumArtist, from.albumArtist);
    str(t.genre, from.genre);
    num(t.track, from.track);
    num(t.trackTotal, from.trackTotal);
    num(t.disc, from.disc);
    num(t.discTotal, from.discTotal);
    num(t.year, from.year);
    if (!t.hasPicture && from.hasPicture) {
        t.picture = from.picture;
        t.hasPicture = true;
    }
}

} // namespace tags

namespace {

enum class Kind { Unknown, Mp3, Flac, Ogg, Wav, Mp4 };

Kind sniff(ByteSource& src)
{
    const std::vector<uint8_t> b = src.read(0, 12);
    const size_t n = b.size();
    if (n < 4) return Kind::Unknown;
    using tags::isAscii;
    if (isAscii(b.data(), n, 0, "fLaC")) return Kind::Flac;
    if (isAscii(b.data(), n, 0, "OggS")) return Kind::Ogg;
    if (isAscii(b.data(), n, 0, "RIFF") && isAscii(b.data(), n, 8, "WAVE")) return Kind::Wav;
    if (isAscii(b.data(), n, 4, "ftyp")) return Kind::Mp4;
    if (isAscii(b.data(), n, 0, "ID3")) {
        // A FLAC can hide behind an ID3v2 tag too.
        const uint64_t size = tags::id3v2Size(b.data(), n);
        const std::vector<uint8_t> after = src.read(size, 4);
        return isAscii(after.data(), after.size(), 0, "fLaC") ? Kind::Flac : Kind::Mp3;
    }
    if (b[0] == 0xff && (b[1] & 0xe0) == 0xe0) return Kind::Mp3;
    return Kind::Unknown;
}

bool readFrom(ByteSource& src, AudioTags& out, std::string* error)
{
    out = AudioTags{};
    bool ok = false;
    switch (sniff(src)) {
        case Kind::Mp3:  ok = tags::readMp3(src, out); break;
        case Kind::Flac: ok = tags::readFlac(src, out); break;
        case Kind::Ogg:  ok = tags::readOgg(src, out); break;
        case Kind::Wav:  ok = tags::readWav(src, out); break;
        case Kind::Mp4:  ok = tags::readMp4(src, out); break;
        default: break;
    }
    if (!ok) {
        out = AudioTags{};
        if (error) *error = "not a recognised audio file (MP3, FLAC, Ogg, WAV, MP4/M4A)";
    }
    return ok;
}

} // namespace

bool readAudioTags(const char* path, AudioTags& out, std::string* error)
{
    ByteSource src;
    if (!src.openFile(path)) {
        out = AudioTags{};
        if (error) *error = std::string("cannot open file: ") + (path ? path : "");
        return false;
    }
    return readFrom(src, out, error);
}

bool readAudioTagsFromMemory(const uint8_t* data, size_t size, AudioTags& out, std::string* error)
{
    ByteSource src;
    src.openMemory(data, size);
    return readFrom(src, out, error);
}

} // namespace broaudio
