// The tag reader's Matroska / WebM half (src/io/tags/matroska.cpp) over
// files built here as EBML bytes: flat (ffmpeg-style) and levelled tags, a
// track-targeted BPS, attachments as the cover, the SeekHead followed past
// the Clusters, a live recording's unknown sizes, and damaged input (every
// truncation, flipped bytes, sizes past the file); and one real encoder's
// file (tests/fixtures/tone_opus.mka: ffmpeg, Opus, tags + a PNG attachment).

#include "test_harness.h"
#include "broaudio/io/audio_tags.h"

#include <cstring>
#include <initializer_list>
#include <limits>
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

Bytes idBytes(uint32_t id)
{
    Bytes b;
    for (int s = 24; s >= 0; s -= 8)
        if (uint8_t(id >> s) || !b.empty()) b.push_back(uint8_t(id >> s));
    return b;
}
// An element with an 8-byte size (always valid EBML), or an unknown size.
Bytes el(uint32_t id, const Bytes& body)
{
    Bytes out = idBytes(id);
    out.push_back(0x01);
    for (int s = 48; s >= 0; s -= 8) out.push_back(uint8_t(uint64_t(body.size()) >> s));
    return cat({out, body});
}
Bytes elUnknown(uint32_t id, const Bytes& body)
{
    Bytes out = idBytes(id);
    out.insert(out.end(), {0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    return cat({out, body});
}
// An element whose size claims more than is there.
Bytes elClaim(uint32_t id, uint64_t size, const Bytes& body)
{
    Bytes out = idBytes(id);
    out.push_back(0x01);
    for (int s = 48; s >= 0; s -= 8) out.push_back(uint8_t(size >> s));
    return cat({out, body});
}
Bytes u(uint32_t id, uint64_t v)
{
    Bytes b;
    for (int s = 56; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
    return el(id, b);
}
Bytes f32(uint32_t id, float v)
{
    uint32_t x;
    std::memcpy(&x, &v, 4);
    return el(id, {uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x)});
}
Bytes f64(uint32_t id, double v)
{
    uint64_t x;
    std::memcpy(&x, &v, 8);
    Bytes b;
    for (int s = 56; s >= 0; s -= 8) b.push_back(uint8_t(x >> s));
    return el(id, b);
}
Bytes s(uint32_t id, const std::string& v) { return el(id, str(v)); }

Bytes ebmlHeader(const std::string& docType) { return el(0x1A45DFA3, cat({u(0x4286, 1), s(0x4282, docType)})); }
Bytes info(double duration, uint64_t scale = 1000000)
{
    return el(0x1549A966, cat({u(0x2AD7B1, scale), f64(0x4489, duration), s(0x4D80, "test")}));
}
Bytes videoTrack() { return el(0xAE, cat({u(0xD7, 1), u(0x73C5, 101), u(0x83, 1), s(0x86, "V_VP9")})); }
Bytes audioTrack(const std::string& codec, float rate, uint64_t channels, uint64_t uid = 202)
{
    return el(0xAE, cat({u(0xD7, 2), u(0x73C5, uid), u(0x83, 2), s(0x86, codec),
                         el(0xE1, cat({f32(0xB5, rate), u(0x9F, channels)}))}));
}
Bytes simple(const std::string& name, const std::string& value)
{
    return el(0x67C8, cat({s(0x45A3, name), s(0x447A, "und"), s(0x4487, value)}));
}
Bytes tag(const Bytes& targets, std::initializer_list<Bytes> simples)
{
    Bytes body = el(0x63C0, targets);
    for (const auto& sb : simples) body = cat({body, sb});
    return el(0x7373, body);
}
Bytes attachment(const std::string& name, const std::string& mime, const Bytes& data)
{
    return el(0x61A7, cat({s(0x466E, name), s(0x4660, mime), el(0x465C, data), u(0x46AE, 7)}));
}
Bytes cluster(size_t payload) { return el(0x1F43B675, cat({u(0xE7, 0), el(0xA3, Bytes(payload, 0x55))})); }

// A Segment whose first child is a SeekHead pointing at `targets` (indices into
// `children`, each with its element id).
Bytes segmentWithSeek(const std::vector<Bytes>& children, const std::vector<std::pair<uint32_t, size_t>>& targets)
{
    auto seekHead = [&](const std::vector<uint64_t>& pos) {
        Bytes body;
        for (size_t i = 0; i < targets.size(); ++i)
            body = cat({body, el(0x4DBB, cat({el(0x53AB, idBytes(targets[i].first)), u(0x53AC, pos[i])}))});
        return el(0x114D9B74, body);
    };
    const size_t headLen = seekHead(std::vector<uint64_t>(targets.size(), 0)).size();
    std::vector<uint64_t> starts;
    uint64_t at = headLen;
    for (const auto& c : children) { starts.push_back(at); at += c.size(); }
    std::vector<uint64_t> pos;
    for (const auto& t : targets) pos.push_back(starts[t.second]);
    Bytes body = seekHead(pos);
    for (const auto& c : children) body = cat({body, c});
    return el(0x18538067, body);
}

bool read(const Bytes& b, AudioTags& t) { return readAudioTagsFromMemory(b.data(), b.size(), t); }

const Bytes kPng = {0x89, 0x50, 0x4e, 0x47, 13, 10, 26, 10, 1, 2, 3, 4};
const Bytes kJpeg = {0xff, 0xd8, 0xff, 0xe0, 9, 8, 7, 6};

// WebM: video then Vorbis audio, flat tags after two Clusters (reached by the
// SeekHead), two pictures of which "cover.jpg" is the front cover.
Bytes flatWebm()
{
    const Bytes tags = el(0x1254C367, cat({
        tag(Bytes{}, {simple("TITLE", "Web Title"), simple("ARTIST", "One"), simple("ARTIST", "Two"),
                      simple("ALBUM", "Web Album"), simple("ALBUM_ARTIST", "Web Band"), simple("PART_NUMBER", "4/10"),
                      simple("DISC", "1/2"), simple("DATE", "2018-07-01"), simple("GENRE", "(17)")}),
        // Tags of one track or chapter are not the file's.
        tag(u(0x63C5, 202), {simple("TITLE", "Track-scoped"), simple("BPS", "96000")}),
        tag(u(0x63C4, 5), {simple("TITLE", "Chapter-scoped")}),
    }));
    const Bytes atts = el(0x1941A469, cat({attachment("small_cover.png", "image/png", kPng),
                                           attachment("cover.jpg", "image/jpeg", kJpeg),
                                           attachment("font.ttf", "font/ttf", Bytes(64, 1))}));
    const std::vector<Bytes> children = {
        info(2500.0), el(0x1654AE6B, cat({videoTrack(), audioTrack("A_VORBIS", 44100.0f, 2)})),
        cluster(300), cluster(300), tags, atts,
    };
    return cat({ebmlHeader("webm"), segmentWithSeek(children, {{0x1549A966, 0}, {0x1654AE6B, 1},
                                                               {0x1254C367, 4}, {0x1941A469, 5}})});
}

} // namespace

TEST(webm_flat_tags_past_the_clusters)
{
    AudioTags t;
    ASSERT_TRUE(read(flatWebm(), t));
    ASSERT_EQ(t.container, std::string("webm"));
    ASSERT_EQ(t.title, std::string("Web Title"));
    ASSERT_EQ(t.artist, std::string("One, Two"));
    ASSERT_EQ(t.album, std::string("Web Album"));
    ASSERT_EQ(t.albumArtist, std::string("Web Band"));
    ASSERT_EQ(t.track, 4);
    ASSERT_EQ(t.trackTotal, 10);
    ASSERT_EQ(t.disc, 1);
    ASSERT_EQ(t.discTotal, 2);
    ASSERT_EQ(t.year, 2018);
    ASSERT_EQ(t.genre, std::string("Rock"));
    ASSERT_NEAR(t.duration, 2.5, 1e-9);
    ASSERT_EQ(t.codec, std::string("vorbis"));
    ASSERT_EQ(t.sampleRate, 44100);
    ASSERT_EQ(t.channels, 2);
    ASSERT_EQ(t.bitrate, 96000);            // the audio track's BPS statistic
    ASSERT_TRUE(t.hasPicture);
    ASSERT_EQ(t.picture.mime, std::string("image/jpeg"));
    ASSERT_EQ(t.picture.type, 3);
    ASSERT_TRUE(t.picture.bytes == kJpeg);
    PASS();
}

TEST(matroska_levelled_tags)
{
    const Bytes tags = el(0x1254C367, cat({
        tag(u(0x68CA, 70), {simple("TOTAL_PARTS", "3")}),
        tag(u(0x68CA, 50), {simple("TITLE", "The Album"), simple("ARTIST", "The Band"), simple("TOTAL_PARTS", "11"),
                            simple("PART_NUMBER", "2"), simple("DATE_RELEASED", "1999")}),
        tag(u(0x68CA, 30), {simple("TITLE", "The Song"), simple("PART_NUMBER", "7"), simple("GENRE", "Jazz")}),
    }));
    // No SeekHead: Info, Tracks and Tags in a row, a Cluster between (skipped by size).
    const Bytes seg = el(0x18538067, cat({info(90000.0, 100000), el(0x1654AE6B, audioTrack("A_AAC/MPEG4/LC", 24000.0f, 1)),
                                          cluster(1000), tags}));
    AudioTags t;
    ASSERT_TRUE(read(cat({ebmlHeader("matroska"), seg}), t));
    ASSERT_EQ(t.container, std::string("matroska"));
    ASSERT_EQ(t.title, std::string("The Song"));
    ASSERT_EQ(t.artist, std::string("The Band"));     // the album's, as the track names none
    ASSERT_EQ(t.album, std::string("The Album"));
    ASSERT_EQ(t.albumArtist, std::string("The Band"));
    ASSERT_EQ(t.track, 7);
    ASSERT_EQ(t.trackTotal, 11);
    ASSERT_EQ(t.disc, 2);
    ASSERT_EQ(t.discTotal, 3);
    ASSERT_EQ(t.year, 1999);
    ASSERT_EQ(t.genre, std::string("Jazz"));
    ASSERT_NEAR(t.duration, 9.0, 1e-9);                // 90000 x 100 us
    ASSERT_EQ(t.codec, std::string("aac"));
    ASSERT_EQ(t.sampleRate, 24000);
    ASSERT_EQ(t.channels, 1);
    ASSERT_FALSE(t.hasPicture);
    PASS();
}

// MediaRecorder-style: unknown-size Segment and Cluster, no Duration. The
// header's tracks are read; nothing past the Cluster can be found, so the
// Tags there are not looked for.
TEST(live_recording_unknown_sizes)
{
    const Bytes infoNoDuration = el(0x1549A966, cat({u(0x2AD7B1, 1000000), s(0x4D80, "Chrome")}));
    const Bytes seg = elUnknown(0x18538067, cat({infoNoDuration, el(0x1654AE6B, cat({audioTrack("A_OPUS", 48000.0f, 1), videoTrack()})),
                                                 elUnknown(0x1F43B675, cat({u(0xE7, 0), el(0xA3, Bytes(500, 1))})),
                                                 el(0x1254C367, tag(Bytes{}, {simple("TITLE", "unreachable")}))}));
    AudioTags t;
    ASSERT_TRUE(read(cat({ebmlHeader("webm"), seg}), t));
    ASSERT_EQ(t.codec, std::string("opus"));
    ASSERT_EQ(t.sampleRate, 48000);
    ASSERT_EQ(t.channels, 1);
    ASSERT_EQ(t.duration, 0.0);
    ASSERT_EQ(t.title, std::string(""));
    PASS();
}

// A video with no audio track: its length, no codec.
TEST(video_only)
{
    const Bytes seg = el(0x18538067, cat({info(1234.0), el(0x1654AE6B, videoTrack()), cluster(64)}));
    AudioTags t;
    ASSERT_TRUE(read(cat({ebmlHeader("webm"), seg}), t));
    ASSERT_NEAR(t.duration, 1.234, 1e-9);
    ASSERT_EQ(t.codec, std::string(""));
    ASSERT_EQ(t.sampleRate, 0);
    PASS();
}

TEST(not_matroska_and_bad_sizes)
{
    AudioTags t;
    ASSERT_FALSE(read(cat({ebmlHeader("dvd"), el(0x18538067, info(1.0))}), t));    // another EBML DocType
    ASSERT_FALSE(read(Bytes{0x1a, 0x45, 0xdf, 0xa3}, t));                           // a bare magic
    ASSERT_FALSE(read(Bytes{0x1a, 0x45, 0xdf, 0xa3, 0x00, 0x00, 0x00, 0x00}, t));   // a zero size marker

    // A Cluster (and a Segment) claiming far more than the file holds: what is before it still reads.
    const Bytes seg = elClaim(0x18538067, 1ull << 40, cat({info(500.0), el(0x1654AE6B, audioTrack("A_FLAC", 96000.0f, 2)),
                                                          elClaim(0x1F43B675, 1ull << 36, Bytes(10, 0))}));
    ASSERT_TRUE(read(cat({ebmlHeader("matroska"), seg}), t));
    ASSERT_NEAR(t.duration, 0.5, 1e-9);
    ASSERT_EQ(t.codec, std::string("flac"));
    ASSERT_EQ(t.sampleRate, 96000);

    // A SeekHead pointing past the Segment, or at the wrong element, is ignored.
    const Bytes wrong = cat({ebmlHeader("webm"),
                             segmentWithSeek({info(10.0), cluster(10)}, {{0x1254C367, 1}, {0x1941A469, 0}})});
    ASSERT_TRUE(read(wrong, t));
    ASSERT_NEAR(t.duration, 0.01, 1e-9);
    ASSERT_EQ(t.title, std::string(""));

    // Non-finite and absurd values come out as 0.
    const Bytes nan = cat({ebmlHeader("webm"), el(0x18538067, cat({el(0x1549A966, f64(0x4489, std::numeric_limits<double>::quiet_NaN())),
                                                                   el(0x1654AE6B, audioTrack("A_OPUS", -5.0f, 1u << 30))}))});
    ASSERT_TRUE(read(nan, t));
    ASSERT_EQ(t.duration, 0.0);
    ASSERT_EQ(t.sampleRate, 0);
    ASSERT_EQ(t.channels, 0);
    PASS();
}

// Every prefix of a file, and that file with bytes flipped: never a crash or
// a read past the bytes (ASan/the debug heap would see one).
TEST(damaged_input)
{
    const Bytes good = flatWebm();
    AudioTags t;
    for (size_t n = 0; n < good.size(); ++n) {
        Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        read(cut, t);
    }
    uint32_t rng = 12345;
    for (int round = 0; round < 2000; ++round) {
        Bytes bad = good;
        for (int k = 0; k < 4; ++k) {
            rng = rng * 1664525u + 1013904223u;
            const size_t at = 4 + (rng >> 8) % (bad.size() - 4);   // keep the magic, so the reader runs
            rng = rng * 1664525u + 1013904223u;
            bad[at] = uint8_t(rng >> 24);
        }
        read(bad, t);
    }
    PASS();
}

// ffmpeg's own Matroska: 0.5 s stereo Opus, global tags, a PNG attachment,
// CRC-32 elements in each master, and Tags after the Cluster behind a SeekHead.
TEST(mka_from_an_encoder)
{
    AudioTags t;
    std::string err;
    const std::string path = std::string(BROAUDIO_TEST_FIXTURES) + "/tone_opus.mka";
    ASSERT_TRUE(readAudioTags(path.c_str(), t, &err));
    ASSERT_EQ(t.container, std::string("matroska"));
    ASSERT_EQ(t.title, std::string("MKA Title"));
    ASSERT_EQ(t.artist, std::string("MKA Artist"));
    ASSERT_EQ(t.album, std::string("MKA Album"));
    ASSERT_EQ(t.albumArtist, std::string("MKA Band"));
    ASSERT_EQ(t.track, 3);
    ASSERT_EQ(t.trackTotal, 12);
    ASSERT_EQ(t.year, 2020);
    ASSERT_EQ(t.genre, std::string("Jazz"));
    ASSERT_EQ(t.codec, std::string("opus"));
    ASSERT_EQ(t.sampleRate, 48000);
    ASSERT_EQ(t.channels, 2);
    ASSERT_NEAR(t.duration, 0.5, 0.025);
    ASSERT_TRUE(t.hasPicture);
    ASSERT_EQ(t.picture.mime, std::string("image/png"));
    ASSERT_EQ(t.picture.type, 3);
    ASSERT_TRUE(t.picture.bytes.size() > 8 && t.picture.bytes[1] == 'P' && t.picture.bytes[2] == 'N');
    PASS();
}

int main() { return runAllTests(); }
