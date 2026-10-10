// MP4 / M4A: the top-level boxes are walked by their headers (mdat is never
// read), moov is read whole and parsed in memory: mvhd, the first sound
// track's edit list, mdhd, hdlr and sample entry (with its esds), and the
// iTunes-style tags in udta/meta/ilst.

#include "tags_internal.h"

#include <algorithm>
#include <cstring>

namespace broaudio::tags {

namespace {

constexpr uint64_t kMaxMoov = 64u * 1024u * 1024u;

constexpr uint32_t fourcc(const char (&s)[5])
{
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

// The '©' items are 0xA9 followed by three letters.
constexpr uint32_t itunes(const char (&s)[4])
{
    return (0xa9u << 24) | (uint32_t(uint8_t(s[0])) << 16) | (uint32_t(uint8_t(s[1])) << 8) | uint32_t(uint8_t(s[2]));
}

struct Box {
    uint32_t type = 0;
    const uint8_t* body = nullptr;
    size_t size = 0;
};

// The child boxes of [b, b+n).
template <typename F>
void eachBox(const uint8_t* b, size_t n, F&& f)
{
    size_t o = 0;
    while (o + 8 <= n) {
        uint64_t size = be32(b + o);
        const uint32_t type = be32(b + o + 4);
        size_t hdr = 8;
        if (size == 1) {
            if (o + 16 > n) return;
            size = be64(b + o + 8);
            hdr = 16;
        } else if (size == 0) {
            size = n - o;
        }
        if (size < hdr || size > n - o) return;
        Box box{type, b + o + hdr, static_cast<size_t>(size - hdr)};
        f(box);
        o += static_cast<size_t>(size);
    }
}

struct Track {
    uint32_t handler = 0;
    Mp4AudioInfo info;
};

// A descriptor's length: up to four bytes of seven bits.
size_t descLen(const uint8_t* b, size_t n, size_t& o)
{
    size_t len = 0;
    for (int i = 0; i < 4 && o < n; ++i) {
        const uint8_t c = b[o++];
        len = (len << 7) | (c & 0x7f);
        if (!(c & 0x80)) break;
    }
    return len;
}

void parseEsds(const uint8_t* b, size_t n, Mp4AudioInfo& info)
{
    size_t o = 4;                                         // version + flags
    if (o >= n || b[o++] != 0x03) return;                 // ES_Descriptor
    descLen(b, n, o);
    if (o + 3 > n) return;
    const uint8_t flags = b[o + 2];
    o += 3;
    if (flags & 0x80) o += 2;                             // dependsOn_ES_ID
    if (flags & 0x40) { if (o >= n) return; o += 1 + b[o]; }   // URL
    if (flags & 0x20) o += 2;                             // OCR_ES_Id
    if (o >= n || b[o++] != 0x04) return;                 // DecoderConfigDescriptor
    const size_t dcLen = descLen(b, n, o);
    const size_t dcEnd = std::min(n, o + dcLen);
    if (o + 13 > dcEnd) return;
    info.objectType = b[o];
    info.avgBitrate = be32(b + o + 9);
    o += 13;
    if (o < dcEnd && b[o] == 0x05) {                      // DecoderSpecificInfo: the AudioSpecificConfig
        ++o;
        const size_t len = descLen(b, n, o);
        if (o + len <= n) info.asc.assign(b + o, b + o + len);
    }
}

void parseSampleEntry(const Box& e, Mp4AudioInfo& info)
{
    info.format = e.type;
    // SampleEntry (6 reserved + data_reference_index) + AudioSampleEntry.
    if (e.size < 28) return;
    const uint8_t* b = e.body;
    const uint32_t version = be16(b + 8);
    info.channels = static_cast<int>(be16(b + 16));
    info.sampleRate = static_cast<int>(be32(b + 24) >> 16);
    size_t childAt = 28;
    if (version == 1) childAt += 16;                      // QuickTime sound description v1
    else if (version == 2) childAt += 36;
    if (childAt > e.size) return;
    auto scan = [&](auto&& self, const uint8_t* cb, size_t cn) -> void {
        eachBox(cb, cn, [&](const Box& c) {
            if (c.type == fourcc("esds")) parseEsds(c.body, c.size, info);
            else if (c.type == fourcc("wave")) self(self, c.body, c.size);   // QuickTime wraps esds in 'wave'
        });
    };
    scan(scan, b + childAt, e.size - childAt);
}

void parseTrak(const Box& trak, Track& tr)
{
    eachBox(trak.body, trak.size, [&](const Box& b) {
        if (b.type == fourcc("edts")) {
            eachBox(b.body, b.size, [&](const Box& el) {
                if (el.type != fourcc("elst") || el.size < 8) return;
                const uint8_t v = el.body[0];
                const uint32_t count = be32(el.body + 4);
                size_t o = 8;
                const size_t entry = v == 1 ? 20 : 12;
                for (uint32_t i = 0; i < count && o + entry <= el.size; ++i, o += entry) {
                    const uint64_t dur = v == 1 ? be64(el.body + o) : be32(el.body + o);
                    const int64_t mt = v == 1 ? static_cast<int64_t>(be64(el.body + o + 8))
                                              : static_cast<int32_t>(be32(el.body + o + 4));
                    if (mt < 0) continue;                // an empty edit (a delay)
                    tr.info.editMediaTime = mt;
                    tr.info.editDuration = dur;
                    break;
                }
            });
        } else if (b.type == fourcc("mdia")) {
            eachBox(b.body, b.size, [&](const Box& m) {
                if (m.type == fourcc("mdhd") && m.size >= 24) {
                    if (m.body[0] == 1 && m.size >= 32) {
                        tr.info.mediaTimescale = be32(m.body + 20);
                        tr.info.mediaDuration = be64(m.body + 24);
                    } else {
                        tr.info.mediaTimescale = be32(m.body + 12);
                        tr.info.mediaDuration = be32(m.body + 16);
                    }
                } else if (m.type == fourcc("hdlr") && m.size >= 12) {
                    tr.handler = be32(m.body + 8);
                } else if (m.type == fourcc("minf")) {
                    eachBox(m.body, m.size, [&](const Box& mi) {
                        if (mi.type != fourcc("stbl")) return;
                        eachBox(mi.body, mi.size, [&](const Box& st) {
                            if (st.type != fourcc("stsd") || st.size < 8) return;
                            bool first = true;
                            eachBox(st.body + 8, st.size - 8, [&](const Box& e) {
                                if (first) parseSampleEntry(e, tr.info);
                                first = false;
                            });
                        });
                    });
                }
            });
        }
    });
}

std::string itemText(uint32_t type, const uint8_t* v, size_t n)
{
    if (type == 2) return trim(utf16ToUtf8(v, n, true));
    return trim(utf8Clean(v, n));
}

void parseIlst(const Box& ilst, AudioTags& t)
{
    eachBox(ilst.body, ilst.size, [&](const Box& item) {
        eachBox(item.body, item.size, [&](const Box& d) {
            if (d.type != fourcc("data") || d.size < 8) return;
            const uint32_t type = be32(d.body) & 0xffffff;
            const uint8_t* v = d.body + 8;
            const size_t n = d.size - 8;
            switch (item.type) {
                case itunes("nam"): if (t.title.empty()) t.title = itemText(type, v, n); break;
                case itunes("ART"): if (t.artist.empty()) t.artist = itemText(type, v, n); break;
                case itunes("alb"): if (t.album.empty()) t.album = itemText(type, v, n); break;
                case fourcc("aART"): if (t.albumArtist.empty()) t.albumArtist = itemText(type, v, n); break;
                case itunes("gen"): if (t.genre.empty()) t.genre = genreName(itemText(type, v, n)); break;
                case itunes("day"): if (!t.year) t.year = yearOf(itemText(type, v, n)); break;
                case fourcc("gnre"):
                    if (t.genre.empty() && n >= 2) t.genre = id3v1Genre(static_cast<int>(be16(v)) - 1);
                    break;
                case fourcc("trkn"):
                    if (n >= 6) { t.track = static_cast<int>(be16(v + 2)); t.trackTotal = static_cast<int>(be16(v + 4)); }
                    break;
                case fourcc("disk"):
                    if (n >= 6) { t.disc = static_cast<int>(be16(v + 2)); t.discTotal = static_cast<int>(be16(v + 4)); }
                    break;
                case fourcc("covr"): {
                    if (t.hasPicture || n < 4) break;
                    AudioTags::Picture pic;
                    if (type == 14 || (v[0] == 0x89 && v[1] == 'P')) pic.mime = "image/png";
                    else if (type == 27 || (v[0] == 'B' && v[1] == 'M')) pic.mime = "image/bmp";
                    else if (type == 13 || (v[0] == 0xff && v[1] == 0xd8)) pic.mime = "image/jpeg";
                    else pic.mime = "image/jpeg";
                    pic.bytes.assign(v, v + n);
                    offerPicture(t, std::move(pic));
                    break;
                }
                default: break;
            }
        });
    });
}

// meta is a FullBox in ISO files but a plain box in QuickTime ones.
void parseMeta(const Box& meta, AudioTags& t)
{
    size_t skip = 4;
    if (meta.size >= 8 && isAscii(meta.body, meta.size, 4, "hdlr")) skip = 0;
    if (meta.size < skip) return;
    eachBox(meta.body + skip, meta.size - skip, [&](const Box& b) {
        if (b.type == fourcc("ilst")) parseIlst(b, t);
    });
}

void parseMoov(const uint8_t* b, size_t n, Mp4AudioInfo& info, AudioTags* tags)
{
    uint32_t movieTs = 0;
    uint64_t movieDur = 0;
    bool haveTrack = false;
    eachBox(b, n, [&](const Box& box) {
        if (box.type == fourcc("mvhd") && box.size >= 20) {
            if (box.body[0] == 1 && box.size >= 32) {
                movieTs = be32(box.body + 20);
                movieDur = be64(box.body + 24);
            } else {
                movieTs = be32(box.body + 12);
                movieDur = be32(box.body + 16);
            }
        } else if (box.type == fourcc("trak") && !haveTrack) {
            Track tr;
            parseTrak(box, tr);
            if (tr.handler == fourcc("soun")) {
                info = tr.info;
                info.found = true;
                haveTrack = true;
            }
        } else if (tags && box.type == fourcc("udta")) {
            eachBox(box.body, box.size, [&](const Box& u) {
                if (u.type == fourcc("meta")) parseMeta(u, *tags);
            });
        } else if (tags && box.type == fourcc("meta")) {
            parseMeta(box, *tags);
        }
    });
    info.movieTimescale = movieTs;
    info.movieDuration = movieDur;
}

} // namespace

bool readMp4Info(ByteSource& src, Mp4AudioInfo& info, AudioTags* tags)
{
    info = Mp4AudioInfo{};
    uint8_t head[16];
    if (src.read(0, head, 8) < 8 || !isAscii(head, 8, 4, "ftyp")) return false;
    uint64_t o = 0;
    bool moov = false;
    uint64_t mdat = 0;
    for (int guard = 0; guard < 4096 && o + 8 <= src.size(); ++guard) {
        if (src.read(o, head, 16) < 8) break;
        uint64_t size = be32(head);
        const uint32_t type = be32(head + 4);
        uint64_t hdr = 8;
        if (size == 1) {
            size = be64(head + 8);
            hdr = 16;
        } else if (size == 0) {
            size = src.size() - o;
        }
        if (size < hdr) break;
        if (type == fourcc("moov") && !moov) {
            if (size - hdr > kMaxMoov) return false;
            const std::vector<uint8_t> b = src.read(o + hdr, static_cast<size_t>(size - hdr));
            Mp4AudioInfo found;
            parseMoov(b.data(), b.size(), found, tags);
            const uint64_t keepMdat = info.mdatBytes;
            info = std::move(found);
            info.mdatBytes = keepMdat;
            moov = true;
        } else if (type == fourcc("mdat")) {
            mdat += size - hdr;
        }
        o += size;
    }
    info.mdatBytes = mdat;
    return moov;
}

bool readMp4(ByteSource& src, AudioTags& t)
{
    Mp4AudioInfo info;
    if (!readMp4Info(src, info, &t)) return false;
    t.container = "mp4";
    if (!info.found) return true;                         // a video, or an empty movie: tags only

    switch (info.format) {
        case fourcc("mp4a"):
            if (info.objectType == 0x40 || (info.objectType >= 0x66 && info.objectType <= 0x68) || info.objectType == 0)
                t.codec = "aac";
            else if (info.objectType == 0x69 || info.objectType == 0x6b)
                t.codec = "mp3";
            else
                t.codec = "mp4a";
            break;
        case fourcc("alac"): t.codec = "alac"; break;
        case fourcc("ac-3"): t.codec = "ac3"; break;
        case fourcc("ec-3"): t.codec = "eac3"; break;
        case fourcc("Opus"): t.codec = "opus"; break;
        case fourcc("fLaC"): t.codec = "flac"; break;
        default: {
            char cc[5] = {char(info.format >> 24), char(info.format >> 16), char(info.format >> 8), char(info.format), 0};
            t.codec = cc;
        }
    }
    t.channels = info.channels;
    t.sampleRate = info.sampleRate ? info.sampleRate : static_cast<int>(info.mediaTimescale);

    if (info.editDuration && info.movieTimescale)
        t.duration = double(info.editDuration) / info.movieTimescale;
    else if (info.mediaDuration && info.mediaTimescale)
        t.duration = double(info.mediaDuration - std::min<uint64_t>(info.mediaDuration, info.editMediaTime > 0 ? uint64_t(info.editMediaTime) : 0)) / info.mediaTimescale;
    else if (info.movieDuration && info.movieTimescale)
        t.duration = double(info.movieDuration) / info.movieTimescale;

    if (info.avgBitrate) t.bitrate = static_cast<int>(std::min<uint32_t>(info.avgBitrate, 0x7fffffff));
    else if (t.duration > 0 && info.mdatBytes) t.bitrate = static_cast<int>(double(info.mdatBytes) * 8 / t.duration + 0.5);
    return true;
}

} // namespace broaudio::tags
