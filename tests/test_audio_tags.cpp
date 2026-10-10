// The tag reader (broaudio/io/audio_tags.h) over fixtures built here as bytes:
// ID3v2.2/2.3/2.4 in every text encoding, unsynchronisation, extended
// headers, ID3v1, MP3 lengths (Xing, VBRI, CBR, a VBR frame walk), FLAC
// comments and pictures, Ogg Vorbis/Opus comments (one packet over pages, a
// base64 picture), WAV INFO and id3 chunks, MP4 ilst atoms and edit lists;
// and one real M4A from an encoder (tests/fixtures/tone_stereo.m4a).
// These are the cases the Music app's JS reader was tested with
// (helmapps/music tests/test_tags_*.js), plus the formats it lacked.

#include "test_harness.h"
#include "broaudio/io/audio_tags.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace broaudio;
using Bytes = std::vector<uint8_t>;

namespace {

Bytes cat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
Bytes str(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes be32(uint32_t n) { return {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)}; }
Bytes be16(uint32_t n) { return {uint8_t(n >> 8), uint8_t(n)}; }
Bytes le32(uint32_t n) { return {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)}; }
Bytes le64(uint64_t n) { return cat({le32(uint32_t(n)), le32(uint32_t(n >> 32))}); }
Bytes ss(uint32_t n) { return {uint8_t((n >> 21) & 127), uint8_t((n >> 14) & 127), uint8_t((n >> 7) & 127), uint8_t(n & 127)}; }
Bytes zeros(size_t n) { return Bytes(n, 0); }

// UTF-8 text to UTF-16 code units (the fixtures are all BMP).
std::vector<uint16_t> units(const std::string& s)
{
    std::vector<uint16_t> u;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        uint32_t cp;
        if (c < 0x80) { cp = c; i += 1; }
        else if ((c >> 5) == 6) { cp = ((c & 0x1f) << 6) | (s[i + 1] & 0x3f); i += 2; }
        else { cp = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f); i += 3; }
        u.push_back(uint16_t(cp));
    }
    return u;
}
Bytes utf16(const std::string& s, bool be, bool bom)
{
    Bytes out;
    if (bom) { if (be) out = {0xfe, 0xff}; else out = {0xff, 0xfe}; }
    for (uint16_t c : units(s)) {
        if (be) { out.push_back(uint8_t(c >> 8)); out.push_back(uint8_t(c)); }
        else { out.push_back(uint8_t(c)); out.push_back(uint8_t(c >> 8)); }
    }
    return out;
}
Bytes latin1(const std::string& s)
{
    Bytes out;
    for (uint16_t c : units(s)) out.push_back(uint8_t(c));
    return out;
}

// --- ID3v2.4 -----------------------------------------------------------------
enum Enc { Latin1 = 0, Utf16 = 1, Utf16be = 2, Utf8 = 3 };

Bytes id3Text(const std::string& id, const std::string& text, Enc e)
{
    Bytes body = e == Utf16 ? utf16(text, false, true) : e == Utf16be ? utf16(text, true, false)
               : e == Utf8 ? str(text) : latin1(text);
    Bytes data = cat({Bytes{uint8_t(e)}, body});
    return cat({str(id), ss(uint32_t(data.size())), Bytes{0, 0}, data});
}
Bytes id3Picture(const Bytes& img, const std::string& mime, int type)
{
    Bytes data = cat({Bytes{0}, str(mime), Bytes{0, uint8_t(type)}, str("cover"), Bytes{0}, img});
    return cat({str("APIC"), ss(uint32_t(data.size())), Bytes{0, 0}, data});
}
Bytes id3v24(std::initializer_list<Bytes> frames, size_t padding = 32)
{
    Bytes body;
    for (const auto& f : frames) body.insert(body.end(), f.begin(), f.end());
    body.resize(body.size() + padding, 0);
    return cat({str("ID3"), Bytes{4, 0, 0}, ss(uint32_t(body.size())), body});
}

// --- MP3 frames ----------------------------------------------------------------
// MPEG-1 layer III, 128 kbps, 44.1 kHz, stereo: 417 bytes.
Bytes mp3Frame(uint8_t rateByte = 0x90)
{
    Bytes f(417, 0);
    f[0] = 0xff; f[1] = 0xfb; f[2] = rateByte; f[3] = 0x64;
    if (rateByte != 0x90) f.resize(rateByte == 0xb0 ? 626 : 313);   // 192 kbps / 96 kbps
    return f;
}
Bytes frames(int n, uint8_t rateByte = 0x90)
{
    Bytes out;
    for (int i = 0; i < n; ++i) { Bytes f = mp3Frame(rateByte); out.insert(out.end(), f.begin(), f.end()); }
    return out;
}

// --- Vorbis comments, FLAC, Ogg --------------------------------------------------
Bytes vorbisComment(std::initializer_list<std::pair<std::string, std::string>> fields)
{
    Bytes out = cat({le32(9), str("helm test"), le32(uint32_t(fields.size()))});
    for (const auto& [k, v] : fields) {
        Bytes e = str(k + "=" + v);
        out = cat({out, le32(uint32_t(e.size())), e});
    }
    return out;
}
Bytes flacWith(const Bytes& comments, double seconds, int rate, std::initializer_list<Bytes> more = {})
{
    Bytes si(34, 0);
    si[0] = 0x10; si[2] = 0x10;
    si[10] = uint8_t((rate >> 12) & 255);
    si[11] = uint8_t((rate >> 4) & 255);
    si[12] = uint8_t(((rate & 15) << 4) | (1 << 1));                 // 2 channels
    const uint64_t total = uint64_t(seconds * rate + 0.5);
    si[13] = uint8_t((15 << 4) | uint8_t(total >> 32));
    Bytes t = be32(uint32_t(total));
    std::memcpy(si.data() + 14, t.data(), 4);
    auto block = [](int type, bool last, const Bytes& body) {
        const uint32_t n = uint32_t(body.size());
        return cat({Bytes{uint8_t((last ? 0x80 : 0) | type), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)}, body});
    };
    Bytes out = cat({str("fLaC"), block(0, false, si), block(4, more.size() == 0, comments)});
    size_t i = 0;
    for (const auto& m : more) out = cat({out, block(6, ++i == more.size(), m)});
    return out;
}
Bytes pictureBlock(const Bytes& img, int type, const std::string& mime = "image/png")
{
    return cat({be32(uint32_t(type)), be32(uint32_t(mime.size())), str(mime), be32(0), be32(1), be32(1),
                be32(24), be32(0), be32(uint32_t(img.size())), img});
}
Bytes oggPage(uint32_t serial, uint32_t seq, uint64_t granule, std::initializer_list<Bytes> packets, uint8_t flags = 0)
{
    Bytes lacing;
    Bytes body;
    for (const auto& p : packets) {
        size_t n = p.size();
        while (n >= 255) { lacing.push_back(255); n -= 255; }
        lacing.push_back(uint8_t(n));
        body.insert(body.end(), p.begin(), p.end());
    }
    return cat({str("OggS"), Bytes{0, flags}, le64(granule), le32(serial), le32(seq), le32(0),
                Bytes{uint8_t(lacing.size())}, lacing, body});
}
std::string base64(const Bytes& b)
{
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < b.size(); i += 3) {
        uint32_t v = uint32_t(b[i]) << 16;
        if (i + 1 < b.size()) v |= uint32_t(b[i + 1]) << 8;
        if (i + 2 < b.size()) v |= b[i + 2];
        s += A[(v >> 18) & 63];
        s += A[(v >> 12) & 63];
        s += i + 1 < b.size() ? A[(v >> 6) & 63] : '=';
        s += i + 2 < b.size() ? A[v & 63] : '=';
    }
    return s;
}

// --- WAV -------------------------------------------------------------------------
Bytes riffChunk(const std::string& id, const Bytes& body)
{
    Bytes out = cat({str(id), le32(uint32_t(body.size())), body});
    if (body.size() & 1) out.push_back(0);
    return out;
}
Bytes wav(double seconds, int rate, const Bytes& extra)
{
    Bytes fmt = cat({Bytes{1, 0, 1, 0}, le32(uint32_t(rate)), le32(uint32_t(rate * 2)), Bytes{2, 0, 16, 0}});
    Bytes data(size_t(seconds * rate) * 2, 0);
    Bytes body = cat({str("WAVE"), riffChunk("fmt ", fmt), extra, riffChunk("data", data)});
    return cat({str("RIFF"), le32(uint32_t(body.size())), body});
}

// --- MP4 ---------------------------------------------------------------------------
Bytes box(const std::string& type, const Bytes& body) { return cat({be32(uint32_t(body.size() + 8)), str(type), body}); }
Bytes fullBox(const std::string& type, uint8_t v, const Bytes& body) { return box(type, cat({Bytes{v, 0, 0, 0}, body})); }
Bytes ilstText(const std::string& name, const std::string& text)
{
    return box(name, box("data", cat({be32(1), be32(0), str(text)})));
}
Bytes ilstPair(const std::string& name, int n, int of)
{
    return box(name, box("data", cat({be32(0), be32(0), Bytes{0, 0}, be16(uint32_t(n)), be16(uint32_t(of)), Bytes{0, 0}})));
}
const std::string kCopy = "\xa9";

struct Mp4Opts {
    uint32_t timescale = 44100;
    uint64_t mediaDuration = 0;
    int64_t editMediaTime = -1;
    uint64_t editDuration = 0;   // movie timescale 1000
    bool mdhdV1 = false;
};
Bytes mp4(const Bytes& ilst, const Mp4Opts& o, const Bytes& mdat = zeros(64))
{
    Bytes mvhd = fullBox("mvhd", 0, cat({be32(0), be32(0), be32(1000), be32(uint32_t(o.mediaDuration * 1000 / o.timescale)), zeros(80)}));
    Bytes mdhd = o.mdhdV1
        ? fullBox("mdhd", 1, cat({zeros(16), be32(o.timescale), be32(uint32_t(o.mediaDuration >> 32)), be32(uint32_t(o.mediaDuration)), zeros(4)}))
        : fullBox("mdhd", 0, cat({be32(0), be32(0), be32(o.timescale), be32(uint32_t(o.mediaDuration)), zeros(4)}));
    Bytes hdlr = fullBox("hdlr", 0, cat({be32(0), str("soun"), zeros(12), Bytes{0}}));
    // AudioSpecificConfig: AAC-LC, 44.1 kHz, stereo = 0x12 0x10.
    Bytes esds = fullBox("esds", 0, cat({Bytes{0x03, 25}, Bytes{0, 1, 0}, Bytes{0x04, 17, 0x40, 0x15}, zeros(3),
                                         be32(128000), be32(96000), Bytes{0x05, 2, 0x12, 0x10}, Bytes{0x06, 1, 2}}));
    Bytes mp4a = box("mp4a", cat({zeros(6), be16(1), zeros(8), be16(2), be16(16), zeros(4), be32(o.timescale << 16), esds}));
    Bytes stsd = fullBox("stsd", 0, cat({be32(1), mp4a}));
    Bytes stbl = box("stbl", stsd);
    Bytes minf = box("minf", stbl);
    Bytes mdia = box("mdia", cat({mdhd, hdlr, minf}));
    Bytes trak;
    if (o.editMediaTime >= 0) {
        Bytes elst = fullBox("elst", 0, cat({be32(1), be32(uint32_t(o.editDuration)), be32(uint32_t(o.editMediaTime)), be32(0x10000)}));
        trak = box("trak", cat({box("edts", elst), mdia}));
    } else {
        trak = box("trak", mdia);
    }
    Bytes meta = fullBox("meta", 0, cat({fullBox("hdlr", 0, cat({be32(0), str("mdir"), zeros(12), Bytes{0}})), box("ilst", ilst)}));
    Bytes moov = box("moov", cat({mvhd, trak, box("udta", meta)}));
    return cat({box("ftyp", cat({str("M4A "), be32(0), str("M4A isom")})), box("mdat", mdat), moov});
}

bool read(const Bytes& b, AudioTags& t) { return readAudioTagsFromMemory(b.data(), b.size(), t); }

// "Ünïcode Söng ♪"
const char* const kSong = "\xc3\x9c" "n\xc3\xaf" "code S\xc3\xb6" "ng \xe2\x99\xaa";

const Bytes kPng = {0x89, 0x50, 0x4e, 0x47, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

} // namespace

// ---- ID3v2.4, every text encoding ---------------------------------------------
TEST(id3v24_encodings_and_picture)
{
    Bytes tag = id3v24({
        id3Text("TIT2", kSong, Utf16),                                               // with a byte-order mark
        id3Text("TPE1", "Bj\xc3\xb6rk", Utf16be),
        id3Text("TALB", "Caf\xc3\xa9 Album", Latin1),
        id3Text("TPE2", "Various \xc3\x84rtists", Utf8),
        id3Text("TRCK", "3/12", Latin1),
        id3Text("TPOS", "2/2", Utf8),
        id3Text("TDRC", "2021-05-03", Utf8),
        id3Text("TCON", "(8)", Latin1),
        id3Picture(Bytes{0xff, 0xd8, 9, 9}, "image/jpeg", 4),                      // a back cover first
        id3Picture(kPng, "image/png", 3),
    });
    Bytes mp3 = cat({tag, frames(8)});
    AudioTags t;
    ASSERT_TRUE(read(mp3, t));
    ASSERT_EQ(t.title, std::string(kSong));
    ASSERT_EQ(t.artist, std::string("Bj\xc3\xb6rk"));
    ASSERT_EQ(t.album, std::string("Caf\xc3\xa9 Album"));
    ASSERT_EQ(t.albumArtist, std::string("Various \xc3\x84rtists"));
    ASSERT_EQ(t.track, 3);
    ASSERT_EQ(t.trackTotal, 12);
    ASSERT_EQ(t.disc, 2);
    ASSERT_EQ(t.discTotal, 2);
    ASSERT_EQ(t.year, 2021);
    ASSERT_EQ(t.genre, std::string("Jazz"));
    ASSERT_TRUE(t.hasPicture);
    ASSERT_EQ(t.picture.type, 3);
    ASSERT_EQ(t.picture.mime, std::string("image/png"));
    ASSERT_TRUE(t.picture.bytes == kPng);
    ASSERT_EQ(t.codec, std::string("mp3"));
    ASSERT_EQ(t.container, std::string("mp3"));
    ASSERT_EQ(t.sampleRate, 44100);
    ASSERT_EQ(t.channels, 2);
    ASSERT_EQ(t.bitrate, 128000);
    ASSERT_NEAR(t.duration, 8 * 417 * 8 / 128000.0, 1e-6);   // CBR: the size over the bit rate
    g_testsPassed++;
}

TEST(id3v24_multiple_values_and_unsync)
{
    AudioTags t;
    ASSERT_TRUE(read(cat({id3v24({id3Text("TPE1", std::string("One\0Two", 7), Utf8)}), frames(4)}), t));
    ASSERT_EQ(t.artist, std::string("One, Two"));

    // Whole-tag unsynchronisation in v2.4: FF 00 in the frame data comes back as FF.
    Bytes data = {0, 'A', 0xff, 0x00, 'B'};          // latin-1 "A\xffB" written unsynchronised
    Bytes frame = cat({str("TIT2"), ss(uint32_t(data.size())), Bytes{0, 0}, data});
    Bytes body = cat({frame, zeros(16)});
    Bytes tag = cat({str("ID3"), Bytes{4, 0, 0x80}, ss(uint32_t(body.size())), body});
    ASSERT_TRUE(read(cat({tag, frames(4)}), t));
    ASSERT_EQ(t.title, std::string("A\xc3\xbf" "B"));
    g_testsPassed++;
}

// ---- ID3v2.3: plain sizes, UTF-16 per value, extended header, whole-tag unsync ----
TEST(id3v23)
{
    auto frame23 = [](const std::string& id, uint8_t enc, const Bytes& body) {
        Bytes data = cat({Bytes{enc}, body});
        return cat({str(id), be32(uint32_t(data.size())), Bytes{0, 0}, data});
    };
    Bytes body = cat({frame23("TIT2", 1, utf16("Wide Title", false, true)), frame23("TPE1", 0, str("Plain")),
                      frame23("TRCK", 0, str("7")), frame23("TYER", 0, str("1999"))});
    AudioTags t;
    ASSERT_TRUE(read(cat({str("ID3"), Bytes{3, 0, 0}, ss(uint32_t(body.size())), body, frames(4)}), t));
    ASSERT_EQ(t.title, std::string("Wide Title"));
    ASSERT_EQ(t.artist, std::string("Plain"));
    ASSERT_EQ(t.track, 7);
    ASSERT_EQ(t.year, 1999);

    // An extended header (v2.3 counts the bytes after its size field) and
    // whole-tag unsynchronisation (a picture whose bytes hold FF E0).
    Bytes img = {0xff, 0xd8, 0xff, 0xe0, 1, 2, 3};
    Bytes apic = cat({Bytes{0}, str("image/jpeg"), Bytes{0, 3, 0}, img});
    Bytes plain = cat({Bytes{0, 0, 0, 6, 0, 0, 0, 0, 0, 0}, frame23("TIT2", 0, str("Ext")),
                       cat({str("APIC"), be32(uint32_t(apic.size())), Bytes{0, 0}, apic})});
    Bytes sync;
    for (size_t i = 0; i < plain.size(); ++i) {
        sync.push_back(plain[i]);
        if (plain[i] == 0xff && (i + 1 == plain.size() || plain[i + 1] >= 0xe0 || plain[i + 1] == 0)) sync.push_back(0);
    }
    ASSERT_TRUE(read(cat({str("ID3"), Bytes{3, 0, 0xc0}, ss(uint32_t(sync.size())), sync, frames(4)}), t));
    ASSERT_EQ(t.title, std::string("Ext"));
    ASSERT_TRUE(t.hasPicture);
    ASSERT_TRUE(t.picture.bytes == img);
    g_testsPassed++;
}

TEST(id3v22)
{
    auto frame22 = [](const std::string& id, const Bytes& data) {
        const uint32_t n = uint32_t(data.size());
        return cat({str(id), Bytes{uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)}, data});
    };
    Bytes body = cat({frame22("TT2", cat({Bytes{0}, str("Old Style")})), frame22("TP1", cat({Bytes{0}, str("Twotwo")})),
                      frame22("PIC", cat({Bytes{0}, str("PNG"), Bytes{3}, str("d"), Bytes{0}, kPng}))});
    AudioTags t;
    ASSERT_TRUE(read(cat({str("ID3"), Bytes{2, 0, 0}, ss(uint32_t(body.size())), body, frames(4)}), t));
    ASSERT_EQ(t.title, std::string("Old Style"));
    ASSERT_EQ(t.artist, std::string("Twotwo"));
    ASSERT_EQ(t.picture.mime, std::string("image/png"));
    ASSERT_TRUE(t.picture.bytes == kPng);
    g_testsPassed++;
}

// ---- ID3v1 fills what v2 left empty ----------------------------------------------
TEST(id3v1)
{
    Bytes v1(128, 0);
    auto put = [&](size_t o, const std::string& s) { std::memcpy(v1.data() + o, s.data(), s.size()); };
    put(0, "TAG"); put(3, "Old Title"); put(33, "Old Artist"); put(63, "Old Album"); put(93, "1987");
    v1[125] = 0; v1[126] = 5; v1[127] = 17;
    AudioTags t;
    ASSERT_TRUE(read(cat({frames(8), v1}), t));
    ASSERT_EQ(t.title, std::string("Old Title"));
    ASSERT_EQ(t.artist, std::string("Old Artist"));
    ASSERT_EQ(t.album, std::string("Old Album"));
    ASSERT_EQ(t.year, 1987);
    ASSERT_EQ(t.track, 5);
    ASSERT_EQ(t.genre, std::string("Rock"));
    ASSERT_NEAR(t.duration, 8 * 417 * 8 / 128000.0, 1e-6);   // the v1 tag is not audio

    ASSERT_TRUE(read(cat({id3v24({id3Text("TIT2", "New", Utf8)}), frames(8), v1}), t));
    ASSERT_EQ(t.title, std::string("New"));
    ASSERT_EQ(t.artist, std::string("Old Artist"));
    g_testsPassed++;
}

// ---- MP3 lengths: Xing, VBRI, a VBR file with neither ---------------------------
TEST(mp3_lengths)
{
    Bytes xing = mp3Frame();
    const Bytes x = cat({str("Xing"), be32(3), be32(1000), be32(400000)});
    std::memcpy(xing.data() + 4 + 32, x.data(), x.size());
    AudioTags t;
    ASSERT_TRUE(read(cat({xing, frames(8)}), t));
    ASSERT_NEAR(t.duration, 1000 * 1152 / 44100.0, 1e-6);
    ASSERT_NEAR(t.bitrate, 400000 * 8 / (1000 * 1152 / 44100.0), 2);

    Bytes vbri = mp3Frame();
    const Bytes v = cat({str("VBRI"), Bytes{0, 1, 0, 0, 0, 50}, be32(200000), be32(500)});
    std::memcpy(vbri.data() + 36, v.data(), v.size());
    ASSERT_TRUE(read(cat({vbri, frames(8)}), t));
    ASSERT_NEAR(t.duration, 500 * 1152 / 44100.0, 1e-6);

    // Frames at 128, 192 and 96 kbps with no header: the frame walk counts them.
    Bytes mixed;
    for (int i = 0; i < 30; ++i) {
        const uint8_t rb = i % 3 == 0 ? 0x90 : i % 3 == 1 ? 0xb0 : 0x70;
        Bytes f = mp3Frame(rb);
        mixed.insert(mixed.end(), f.begin(), f.end());
    }
    ASSERT_TRUE(read(mixed, t));
    ASSERT_NEAR(t.duration, 30 * 1152 / 44100.0, 1e-6);
    g_testsPassed++;
}

TEST(not_audio)
{
    AudioTags t;
    std::string err;
    Bytes junk = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    ASSERT_FALSE(readAudioTagsFromMemory(junk.data(), junk.size(), t, &err));
    ASSERT_FALSE(err.empty());
    Bytes zero(64, 0);
    ASSERT_FALSE(read(zero, t));
    ASSERT_TRUE(t.title.empty());
    ASSERT_FALSE(readAudioTags("no/such/file.mp3", t, &err));
    g_testsPassed++;
}

// ---- FLAC ---------------------------------------------------------------------------
TEST(flac)
{
    Bytes vc = vorbisComment({{"TITLE", "Fl\xc3\xbcte Song"}, {"ARTIST", "\xc3\x98rkester"}, {"ALBUM", "\xc3\x9cnder Water"},
                              {"TRACKNUMBER", "4"}, {"TRACKTOTAL", "9"}, {"DISCNUMBER", "1/2"},
                              {"DATE", "2019-04-01"}, {"ALBUMARTIST", "The Band"}, {"GENRE", "Folk"}});
    Bytes f = flacWith(vc, 125.5, 48000);
    AudioTags t;
    ASSERT_TRUE(read(f, t));
    ASSERT_EQ(t.title, std::string("Fl\xc3\xbcte Song"));
    ASSERT_EQ(t.artist, std::string("\xc3\x98rkester"));
    ASSERT_EQ(t.album, std::string("\xc3\x9cnder Water"));
    ASSERT_EQ(t.track, 4);
    ASSERT_EQ(t.trackTotal, 9);
    ASSERT_EQ(t.disc, 1);
    ASSERT_EQ(t.discTotal, 2);
    ASSERT_EQ(t.year, 2019);
    ASSERT_EQ(t.albumArtist, std::string("The Band"));
    ASSERT_EQ(t.genre, std::string("Folk"));
    ASSERT_EQ(t.sampleRate, 48000);
    ASSERT_EQ(t.channels, 2);
    ASSERT_NEAR(t.duration, 125.5, 1e-6);
    ASSERT_EQ(t.codec, std::string("flac"));
    ASSERT_FALSE(t.hasPicture);

    // Keys are case-insensitive and repeated artists join.
    ASSERT_TRUE(read(flacWith(vorbisComment({{"title", "lower"}, {"Artist", "A"}, {"ARTIST", "B"}}), 1, 44100), t));
    ASSERT_EQ(t.title, std::string("lower"));
    ASSERT_EQ(t.artist, std::string("A, B"));

    // PICTURE blocks after the comments: the front cover wins over an earlier back cover.
    ASSERT_TRUE(read(flacWith(vc, 2, 44100, {pictureBlock(Bytes{0xff, 0xd8, 1}, 4, "image/jpeg"), pictureBlock(kPng, 3)}), t));
    ASSERT_TRUE(t.hasPicture);
    ASSERT_EQ(t.picture.type, 3);
    ASSERT_EQ(t.picture.mime, std::string("image/png"));
    ASSERT_TRUE(t.picture.bytes == kPng);

    // An ID3v2 tag in front of fLaC is read past (and fills gaps).
    ASSERT_TRUE(read(cat({id3v24({id3Text("TCON", "Ambient", Utf8)}), f}), t));
    ASSERT_EQ(t.title, std::string("Fl\xc3\xbcte Song"));
    ASSERT_EQ(t.genre, std::string("Folk"));
    ASSERT_NEAR(t.duration, 125.5, 1e-6);
    g_testsPassed++;
}

// ---- Ogg Vorbis and Opus --------------------------------------------------------------
TEST(ogg_vorbis)
{
    Bytes id = cat({Bytes{1}, str("vorbis"), le32(0), Bytes{2}, le32(44100), zeros(13)});
    Bytes tags = cat({Bytes{3}, str("vorbis"),
                      vorbisComment({{"TITLE", "Ogg Title"}, {"ARTIST", "Ogg Artist"}, {"ALBUM", "Ogg Album"}, {"TRACKNUMBER", "2"}}),
                      Bytes{1}});
    Bytes ogg = cat({oggPage(7, 0, 0, {id}, 2), oggPage(7, 1, 0, {tags}), oggPage(7, 2, 44100 * 3, {zeros(40)}, 4)});
    AudioTags t;
    ASSERT_TRUE(read(ogg, t));
    ASSERT_EQ(t.codec, std::string("vorbis"));
    ASSERT_EQ(t.container, std::string("ogg"));
    ASSERT_EQ(t.title, std::string("Ogg Title"));
    ASSERT_EQ(t.artist, std::string("Ogg Artist"));
    ASSERT_EQ(t.album, std::string("Ogg Album"));
    ASSERT_EQ(t.track, 2);
    ASSERT_EQ(t.channels, 2);
    ASSERT_EQ(t.sampleRate, 44100);
    ASSERT_NEAR(t.duration, 3.0, 1e-6);

    // A comment packet over two pages (a big picture does this), the picture base64.
    Bytes block = pictureBlock(kPng, 3);
    Bytes big = cat({Bytes{3}, str("vorbis"),
                     vorbisComment({{"TITLE", "Long"}, {"COMMENT", std::string(600, 'x')},
                                    {"METADATA_BLOCK_PICTURE", base64(block)}}),
                     Bytes{1}});
    const size_t cut = 255 * 2;
    Bytes first(big.begin(), big.begin() + cut);
    Bytes rest(big.begin() + cut, big.end());
    Bytes page2 = cat({str("OggS"), Bytes{0, 0}, le64(0), le32(7), le32(1), le32(0), Bytes{2, 255, 255}, first});
    Bytes spread = cat({oggPage(7, 0, 0, {id}, 2), page2, oggPage(7, 2, 0, {rest}, 1), oggPage(7, 3, 22050, {zeros(10)}, 4)});
    ASSERT_TRUE(read(spread, t));
    ASSERT_EQ(t.title, std::string("Long"));
    ASSERT_TRUE(t.hasPicture);
    ASSERT_TRUE(t.picture.bytes == kPng);
    ASSERT_NEAR(t.duration, 0.5, 1e-6);
    g_testsPassed++;
}

TEST(ogg_opus)
{
    Bytes head = cat({str("OpusHead"), Bytes{1, 2, 0x38, 0x01}, le32(48000), Bytes{0, 0, 0}});
    Bytes tags = cat({str("OpusTags"), vorbisComment({{"TITLE", "Opus Title"}, {"ARTIST", "Opus Artist"}, {"TRACKNUMBER", "5/6"}})});
    Bytes opus = cat({oggPage(9, 0, 0, {head}, 2), oggPage(9, 1, 0, {tags}), oggPage(9, 2, 48000 * 2 + 312, {zeros(20)}, 4)});
    AudioTags t;
    ASSERT_TRUE(read(opus, t));
    ASSERT_EQ(t.codec, std::string("opus"));
    ASSERT_EQ(t.title, std::string("Opus Title"));
    ASSERT_EQ(t.artist, std::string("Opus Artist"));
    ASSERT_EQ(t.track, 5);
    ASSERT_EQ(t.trackTotal, 6);
    ASSERT_EQ(t.channels, 2);
    ASSERT_EQ(t.sampleRate, 48000);
    ASSERT_NEAR(t.duration, 2.0, 1e-6);   // less the pre-skip
    g_testsPassed++;
}

// ---- WAV ---------------------------------------------------------------------------------
TEST(wav)
{
    auto info = [](const std::string& k, const std::string& v) { return riffChunk(k, cat({str(v), Bytes{0}})); };
    Bytes list = riffChunk("LIST", cat({str("INFO"), info("INAM", "Wav Title"), info("IART", "Wav Artist"),
                                        info("IPRD", "Wav Album"), info("ITRK", "11"), info("ICRD", "2003"),
                                        info("IGNR", "Caf\xe9")}));                  // Latin-1, not UTF-8
    AudioTags t;
    ASSERT_TRUE(read(wav(0.25, 8000, list), t));
    ASSERT_EQ(t.container, std::string("wav"));
    ASSERT_EQ(t.codec, std::string("pcm"));
    ASSERT_EQ(t.title, std::string("Wav Title"));
    ASSERT_EQ(t.artist, std::string("Wav Artist"));
    ASSERT_EQ(t.album, std::string("Wav Album"));
    ASSERT_EQ(t.track, 11);
    ASSERT_EQ(t.year, 2003);
    ASSERT_EQ(t.genre, std::string("Caf\xc3\xa9"));
    ASSERT_EQ(t.sampleRate, 8000);
    ASSERT_EQ(t.channels, 1);
    ASSERT_EQ(t.bitrate, 128000);
    ASSERT_NEAR(t.duration, 0.25, 1e-6);

    // An "id3 " chunk.
    Bytes id3 = riffChunk("id3 ", id3v24({id3Text("TIT2", "From ID3", Utf8), id3Picture(kPng, "image/png", 3)}));
    ASSERT_TRUE(read(wav(0.5, 8000, id3), t));
    ASSERT_EQ(t.title, std::string("From ID3"));
    ASSERT_TRUE(t.hasPicture);
    ASSERT_NEAR(t.duration, 0.5, 1e-6);
    g_testsPassed++;
}

// ---- MP4 / M4A ------------------------------------------------------------------------------
TEST(mp4_ilst)
{
    Bytes jpeg = {0xff, 0xd8, 0xff, 0xe0, 7, 7, 7};
    Bytes ilst = cat({ilstText(kCopy + "nam", "M4A Song"), ilstText(kCopy + "ART", "M4A Artist"),
                      ilstText(kCopy + "alb", "M4A Album"), ilstText("aART", "M4A Band"),
                      ilstText(kCopy + "day", "2018-02-03T00:00:00Z"), ilstPair("trkn", 6, 10), ilstPair("disk", 2, 3),
                      box("gnre", box("data", cat({be32(0), be32(0), be16(9)}))),          // ID3v1 index 8 + 1: Jazz
                      box("covr", box("data", cat({be32(13), be32(0), jpeg})))});
    Mp4Opts o;
    o.mediaDuration = 44100 * 4 + 1024 + 300;
    o.editMediaTime = 1024;
    o.editDuration = 4000;
    AudioTags t;
    ASSERT_TRUE(read(mp4(ilst, o), t));
    ASSERT_EQ(t.container, std::string("mp4"));
    ASSERT_EQ(t.codec, std::string("aac"));
    ASSERT_EQ(t.title, std::string("M4A Song"));
    ASSERT_EQ(t.artist, std::string("M4A Artist"));
    ASSERT_EQ(t.album, std::string("M4A Album"));
    ASSERT_EQ(t.albumArtist, std::string("M4A Band"));
    ASSERT_EQ(t.year, 2018);
    ASSERT_EQ(t.track, 6);
    ASSERT_EQ(t.trackTotal, 10);
    ASSERT_EQ(t.disc, 2);
    ASSERT_EQ(t.discTotal, 3);
    ASSERT_EQ(t.genre, std::string("Jazz"));
    ASSERT_TRUE(t.hasPicture);
    ASSERT_EQ(t.picture.mime, std::string("image/jpeg"));
    ASSERT_TRUE(t.picture.bytes == jpeg);
    ASSERT_EQ(t.sampleRate, 44100);
    ASSERT_EQ(t.channels, 2);
    ASSERT_EQ(t.bitrate, 96000);
    ASSERT_NEAR(t.duration, 4.0, 1e-6);   // the edit list, not mdhd

    // No edit list: mdhd (v1, 64-bit) gives the length; ©gen as text.
    Mp4Opts p;
    p.mediaDuration = 44100 * 3;
    p.mdhdV1 = true;
    ASSERT_TRUE(read(mp4(ilstText(kCopy + "gen", "Electronic"), p), t));
    ASSERT_EQ(t.genre, std::string("Electronic"));
    ASSERT_NEAR(t.duration, 3.0, 1e-6);
    ASSERT_FALSE(t.hasPicture);

    // moov after a big mdat: found by walking the box headers.
    ASSERT_TRUE(read(mp4(ilstText(kCopy + "nam", "Late"), p, zeros(300000)), t));
    ASSERT_EQ(t.title, std::string("Late"));
    g_testsPassed++;
}

// A real encoder's M4A (ffmpeg's AAC, 0.5 s stereo, iTunes-style tags).
TEST(m4a_from_an_encoder)
{
    AudioTags t;
    std::string err;
    const std::string path = std::string(BROAUDIO_TEST_FIXTURES) + "/tone_stereo.m4a";
    ASSERT_TRUE(readAudioTags(path.c_str(), t, &err));
    ASSERT_EQ(t.title, std::string("M4A Title"));
    ASSERT_EQ(t.artist, std::string("M4A Artist"));
    ASSERT_EQ(t.album, std::string("M4A Album"));
    ASSERT_EQ(t.albumArtist, std::string("M4A Band"));
    ASSERT_EQ(t.track, 3);
    ASSERT_EQ(t.trackTotal, 12);
    ASSERT_EQ(t.disc, 1);
    ASSERT_EQ(t.discTotal, 2);
    ASSERT_EQ(t.year, 2020);
    ASSERT_EQ(t.genre, std::string("Jazz"));
    ASSERT_EQ(t.codec, std::string("aac"));
    ASSERT_EQ(t.sampleRate, 44100);
    ASSERT_EQ(t.channels, 2);
    ASSERT_NEAR(t.duration, 0.5, 0.001);
    ASSERT_GT(t.bitrate, 30000);
    g_testsPassed++;
}

int main() { return runAllTests(); }
