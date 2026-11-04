// Matroska / WebM (.mkv .mka .webm .weba): the EBML header's DocType, the
// Segment's Info (TimecodeScale x Duration), the first audio track's codec,
// rate and channels, Tags/SimpleTag, and an image attachment as the cover.
//
// Only element headers and the few small values wanted are read. The Segment's
// top level is walked until the first Cluster; what lies past the Clusters
// (Tags, Attachments, often at the end) is reached through the SeekHead, so a
// multi-GB file costs a handful of reads. Every size is bounded by its parent,
// the Segment and the file; an unknown size (a live recording's Cluster) ends
// the walk, since what follows cannot be found without reading it.

#include "tags_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>

namespace broaudio::tags {

namespace {

constexpr uint32_t kEbml = 0x1A45DFA3, kDocType = 0x4282;
constexpr uint32_t kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB;
constexpr uint32_t kSeekId = 0x53AB, kSeekPos = 0x53AC;
constexpr uint32_t kInfo = 0x1549A966, kTimecodeScale = 0x2AD7B1, kDuration = 0x4489, kSegTitle = 0x7BA9;
constexpr uint32_t kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackType = 0x83, kTrackUid = 0x73C5;
constexpr uint32_t kCodecId = 0x86, kAudio = 0xE1, kSamplingFreq = 0xB5, kOutputFreq = 0x78B5, kChannels = 0x9F;
constexpr uint32_t kTags = 0x1254C367, kTag = 0x7373, kTargets = 0x63C0, kTargetTypeValue = 0x68CA;
constexpr uint32_t kTagTrackUid = 0x63C5, kTagEditionUid = 0x63C9, kTagChapterUid = 0x63C4, kTagAttachmentUid = 0x63C6;
constexpr uint32_t kSimpleTag = 0x67C8, kTagName = 0x45A3, kTagString = 0x4487;
constexpr uint32_t kAttachments = 0x1941A469, kAttachedFile = 0x61A7, kFileName = 0x466E;
constexpr uint32_t kFileMediaType = 0x4660, kFileData = 0x465C;
constexpr uint32_t kCluster = 0x1F43B675;

constexpr size_t kMaxString = 64 * 1024;   // a tag value, a name, a codec id
constexpr int kMaxElements = 200000;       // headers read in all, across every walk
constexpr int kMaxClustersWithoutSeek = 64;

struct Elem {
    uint32_t id = 0;
    uint64_t start = 0;   // the header's first byte
    uint64_t data = 0;    // the body's first byte
    uint64_t end = 0;     // one past the body, clamped to the parent
    bool unknownSize = false;
    bool overran = false; // the stated size ran past the parent
};

class Walker {
public:
    explicit Walker(ByteSource& src) : src_(src) {}

    // The element header at `pos` within [pos, parentEnd). False when there is
    // no valid header there (or the budget is spent).
    bool header(uint64_t pos, uint64_t parentEnd, Elem& e)
    {
        if (budget_-- <= 0 || pos >= parentEnd) return false;
        uint8_t b[12];
        const size_t n = src_.read(pos, b, static_cast<size_t>(std::min<uint64_t>(12, parentEnd - pos)));
        if (n < 2) return false;
        // ID: 1-4 bytes, the length marker kept.
        int idLen = 0;
        for (int k = 0; k < 4; ++k)
            if (b[0] & (0x80 >> k)) { idLen = k + 1; break; }
        if (!idLen || static_cast<size_t>(idLen) >= n) return false;
        uint32_t id = 0;
        for (int k = 0; k < idLen; ++k) id = (id << 8) | b[k];
        // Size: 1-8 bytes, the marker dropped; every value bit set is "unknown".
        const uint8_t s0 = b[idLen];
        int sLen = 0;
        for (int k = 0; k < 8; ++k)
            if (s0 & (0x80 >> k)) { sLen = k + 1; break; }
        if (!sLen || static_cast<size_t>(idLen + sLen) > n) return false;
        uint64_t size = s0 & (0xFFu >> sLen);
        bool allOnes = size == (0xFFu >> sLen);
        for (int k = 1; k < sLen; ++k) {
            size = (size << 8) | b[idLen + k];
            allOnes = allOnes && b[idLen + k] == 0xFF;
        }
        e.id = id;
        e.start = pos;
        e.data = pos + static_cast<uint64_t>(idLen + sLen);
        e.unknownSize = allOnes;
        e.overran = false;
        if (allOnes) {
            e.end = parentEnd;
        } else if (size > parentEnd - e.data) {
            e.end = parentEnd;
            e.overran = true;
        } else {
            e.end = e.data + size;
        }
        return true;
    }

    uint64_t uintValue(const Elem& e)
    {
        const uint64_t n = e.end - e.data;
        if (n == 0 || n > 8) return 0;
        uint8_t b[8];
        const size_t got = src_.read(e.data, b, static_cast<size_t>(n));
        uint64_t v = 0;
        for (size_t k = 0; k < got; ++k) v = (v << 8) | b[k];
        return v;
    }

    double floatValue(const Elem& e)
    {
        const uint64_t n = e.end - e.data;
        uint8_t b[8];
        if (n == 4 && src_.read(e.data, b, 4) == 4) {
            const uint32_t u = be32(b);
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
        if (n == 8 && src_.read(e.data, b, 8) == 8) {
            const uint64_t u = be64(b);
            double d;
            std::memcpy(&d, &u, 8);
            return d;
        }
        return 0.0;
    }

    // A string value (ASCII or UTF-8), zero padding dropped; "" past kMaxString.
    std::string stringValue(const Elem& e)
    {
        const uint64_t n = e.end - e.data;
        if (n == 0 || n > kMaxString) return {};
        const std::vector<uint8_t> b = src_.read(e.data, static_cast<size_t>(n));
        size_t len = b.size();
        while (len && b[len - 1] == 0) --len;
        return trim(utf8Clean(b.data(), len));
    }

    std::vector<uint8_t> bytes(const Elem& e, size_t cap)
    {
        const uint64_t n = e.end - e.data;
        if (n == 0 || n > cap) return {};
        return src_.read(e.data, static_cast<size_t>(n));
    }

    // Call fn(child) for each child of [from, to); stops at an unknown size or
    // a header that cannot be read. fn returns false to stop early.
    template <typename Fn>
    void children(uint64_t from, uint64_t to, Fn&& fn)
    {
        Elem c;
        for (uint64_t pos = from; pos < to && header(pos, to, c);) {
            if (!fn(c)) return;
            if (c.unknownSize || c.overran) return;
            pos = c.end;
        }
    }

private:
    ByteSource& src_;
    int budget_ = kMaxElements;
};

std::string upper(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string codecName(const std::string& id)
{
    static const struct { const char* prefix; const char* name; } kCodecs[] = {
        {"A_OPUS", "opus"}, {"A_VORBIS", "vorbis"}, {"A_AAC", "aac"}, {"A_FLAC", "flac"},
        {"A_MPEG/L3", "mp3"}, {"A_MPEG/L2", "mp2"}, {"A_MPEG/L1", "mp1"}, {"A_EAC3", "eac3"},
        {"A_AC3", "ac3"}, {"A_PCM/FLOAT", "float"}, {"A_PCM/INT", "pcm"}, {"A_ALAC", "alac"},
        {"A_DTS", "dts"}, {"A_TRUEHD", "truehd"}, {"A_WAVPACK", "wavpack"}, {"A_TTA", "tta"},
    };
    for (const auto& c : kCodecs)
        if (id.compare(0, std::strlen(c.prefix), c.prefix) == 0) return c.name;
    std::string s = id.compare(0, 2, "A_") == 0 ? id.substr(2) : id;
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The SimpleTags of the file as a whole (no track/chapter/attachment target),
// by TargetTypeValue: 30 and below is the track, 50 the album, 60+ the set.
struct Level {
    std::multimap<std::string, std::string> values;   // TAGNAME -> value
    bool any() const { return !values.empty(); }
    std::string first(std::initializer_list<const char*> names) const
    {
        for (const char* n : names) {
            auto it = values.find(n);
            if (it != values.end()) return it->second;
        }
        return {};
    }
    std::string joined(const char* name) const
    {
        std::string out;
        auto range = values.equal_range(name);
        for (auto it = range.first; it != range.second; ++it) {
            if (!out.empty()) out += ", ";
            out += it->second;
        }
        return out;
    }
};

struct Parsed {
    std::string docType;
    uint64_t timecodeScale = 1000000;
    double duration = 0.0;          // in TimecodeScale units
    std::string segmentTitle;
    bool audio = false;
    std::string codecId;
    double sampleRate = 0.0;
    uint64_t channels = 0;
    uint64_t audioUid = 0;
    Level track, album, set;
    std::map<uint64_t, std::string> bpsByTrack;   // a track-targeted BPS statistic
};

void readInfo(Walker& w, const Elem& info, Parsed& p)
{
    w.children(info.data, info.end, [&](const Elem& c) {
        if (c.id == kTimecodeScale) {
            const uint64_t s = w.uintValue(c);
            if (s) p.timecodeScale = s;
        } else if (c.id == kDuration) {
            p.duration = w.floatValue(c);
        } else if (c.id == kSegTitle) {
            p.segmentTitle = w.stringValue(c);
        }
        return true;
    });
}

void readTracks(Walker& w, const Elem& tracks, Parsed& p)
{
    w.children(tracks.data, tracks.end, [&](const Elem& entry) {
        if (entry.id != kTrackEntry) return true;
        uint64_t type = 0, uid = 0, channels = 1;
        double rate = 8000.0, outRate = 0.0;
        std::string codec;
        w.children(entry.data, entry.end, [&](const Elem& c) {
            if (c.id == kTrackType) type = w.uintValue(c);
            else if (c.id == kTrackUid) uid = w.uintValue(c);
            else if (c.id == kCodecId) codec = w.stringValue(c);
            else if (c.id == kAudio) {
                w.children(c.data, c.end, [&](const Elem& a) {
                    if (a.id == kSamplingFreq) rate = w.floatValue(a);
                    else if (a.id == kOutputFreq) outRate = w.floatValue(a);
                    else if (a.id == kChannels) channels = w.uintValue(a);
                    return true;
                });
            }
            return true;
        });
        if (type != 2 || p.audio) return true;   // the first audio track
        p.audio = true;
        p.codecId = codec;
        p.sampleRate = outRate > 0 ? outRate : rate;
        p.channels = channels;
        p.audioUid = uid;
        return true;
    });
}

void readTags(Walker& w, const Elem& tags, Parsed& p)
{
    w.children(tags.data, tags.end, [&](const Elem& tag) {
        if (tag.id != kTag) return true;
        uint64_t level = 50, trackUid = 0;
        bool otherTarget = false;
        std::vector<std::pair<std::string, std::string>> simple;
        w.children(tag.data, tag.end, [&](const Elem& c) {
            if (c.id == kTargets) {
                w.children(c.data, c.end, [&](const Elem& t) {
                    if (t.id == kTargetTypeValue) level = w.uintValue(t);
                    else if (t.id == kTagTrackUid) { const uint64_t u = w.uintValue(t); if (u) trackUid = u; }
                    else if (t.id == kTagEditionUid || t.id == kTagChapterUid || t.id == kTagAttachmentUid)
                        otherTarget = otherTarget || w.uintValue(t) != 0;
                    return true;
                });
            } else if (c.id == kSimpleTag) {
                std::string name, value;
                w.children(c.data, c.end, [&](const Elem& s) {
                    if (s.id == kTagName) name = upper(w.stringValue(s));
                    else if (s.id == kTagString) value = w.stringValue(s);
                    return true;   // a nested SimpleTag (SORT_WITH, ...) is not this tag's value
                });
                if (!name.empty() && !value.empty()) simple.emplace_back(std::move(name), std::move(value));
            }
            return true;
        });
        if (otherTarget) return true;
        if (trackUid) {
            for (auto& [name, value] : simple)
                if (name == "BPS") p.bpsByTrack[trackUid] = value;
            return true;
        }
        Level& into = level <= 30 ? p.track : level <= 50 ? p.album : p.set;
        for (auto& [name, value] : simple) into.values.emplace(std::move(name), std::move(value));
        return true;
    });
}

void readAttachments(Walker& w, const Elem& att, AudioTags& t)
{
    w.children(att.data, att.end, [&](const Elem& file) {
        if (file.id != kAttachedFile) return true;
        std::string name, mime;
        Elem data;
        bool hasData = false;
        w.children(file.data, file.end, [&](const Elem& c) {
            if (c.id == kFileName) name = w.stringValue(c);
            else if (c.id == kFileMediaType) mime = w.stringValue(c);
            else if (c.id == kFileData && !c.overran) { data = c; hasData = true; }
            return true;
        });
        for (auto& ch : mime) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!hasData || mime.compare(0, 6, "image/") != 0) return true;
        AudioTags::Picture pic;
        pic.mime = mime;
        // Matroska's convention: "cover.*" is the front cover; others are other pictures.
        const std::string lower = [&] {
            std::string s = name;
            for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }();
        pic.type = lower.compare(0, 6, "cover.") == 0 ? 3 : -1;
        pic.bytes = w.bytes(data, kMaxBlock);   // a picture too big to hold is left out, not cut
        offerPicture(t, std::move(pic));
        return true;
    });
}

void applyTags(const Parsed& p, AudioTags& t)
{
    int n = 0, of = 0;
    auto year = [&](const Level& l) {
        if (!t.year) t.year = yearOf(l.first({"DATE_RELEASED", "DATE_RECORDED", "DATE", "YEAR", "DATE_RELEASE"}));
    };
    auto genre = [&](const Level& l) { if (t.genre.empty()) t.genre = l.first({"GENRE"}); };
    if (p.track.any()) {
        // Levelled, as the spec has it: the track's TITLE/ARTIST/PART_NUMBER at
        // 30, the album's TITLE/ARTIST/TOTAL_PARTS (and disc PART_NUMBER) at 50.
        t.title = p.track.first({"TITLE"});
        t.artist = p.track.joined("ARTIST");
        if (t.artist.empty()) t.artist = p.track.first({"LEAD_PERFORMER"});
        numberPair(p.track.first({"PART_NUMBER", "TRACKNUMBER", "TRACK"}), n, of);
        t.track = n;
        if (of) t.trackTotal = of;
        t.album = p.album.first({"TITLE", "ALBUM"});
        t.albumArtist = p.album.joined("ARTIST");
        if (t.artist.empty()) t.artist = t.albumArtist;
        numberPair(p.album.first({"TOTAL_PARTS"}), n, of);
        if (n) t.trackTotal = n;
        numberPair(p.album.first({"PART_NUMBER"}), n, of);
        t.disc = n;
        numberPair(p.set.first({"TOTAL_PARTS"}), n, of);
        t.discTotal = n;
        year(p.track);
        year(p.album);
        genre(p.track);
        genre(p.album);
    } else {
        // Flat, as ffmpeg and most taggers write it: one untargeted Tag with
        // Vorbis-style names (PART_NUMBER is the track number).
        const Level& l = p.album;
        t.title = l.first({"TITLE"});
        t.artist = l.joined("ARTIST");
        if (t.artist.empty()) t.artist = l.first({"LEAD_PERFORMER"});
        t.album = l.first({"ALBUM"});
        t.albumArtist = l.first({"ALBUM_ARTIST", "ALBUMARTIST", "ALBUM ARTIST"});
        numberPair(l.first({"PART_NUMBER", "TRACKNUMBER", "TRACK"}), n, of);
        t.track = n;
        t.trackTotal = of;
        numberPair(l.first({"TOTAL_PARTS", "TRACKTOTAL", "TOTALTRACKS"}), n, of);
        if (n) t.trackTotal = n;
        numberPair(l.first({"DISCNUMBER", "DISC"}), n, of);
        t.disc = n;
        t.discTotal = of;
        numberPair(l.first({"DISCTOTAL", "TOTALDISCS"}), n, of);
        if (n) t.discTotal = n;
        year(l);
        genre(l);
    }
    t.genre = genreName(t.genre);
    if (t.title.empty()) t.title = p.segmentTitle;
}

} // namespace

bool readMatroska(ByteSource& src, AudioTags& t)
{
    Walker w(src);
    const uint64_t fileEnd = src.size();
    Elem e;
    if (!w.header(0, fileEnd, e) || e.id != kEbml || e.unknownSize || e.overran) return false;

    Parsed p;
    p.docType = "matroska";
    w.children(e.data, e.end, [&](const Elem& c) {
        if (c.id == kDocType) p.docType = w.stringValue(c);
        return true;
    });
    if (p.docType != "webm" && p.docType != "matroska") return false;
    t.container = p.docType;

    // The first Segment at the top level (a Void or CRC before it is skipped).
    Elem seg;
    bool found = false;
    w.children(e.end, fileEnd, [&](const Elem& c) {
        if (c.id != kSegment) return true;
        seg = c;
        found = true;
        return false;
    });
    if (!found) return true;   // a Matroska header and no Segment: nothing more to say

    // The top level up to the first Cluster: what is there, and the SeekHead's
    // pointers to what is not.
    std::map<uint32_t, std::vector<uint64_t>> at;   // element id -> header offsets
    std::vector<uint64_t> seekHeads;
    auto note = [&](uint32_t id, uint64_t pos) {
        auto& v = at[id];
        if (std::find(v.begin(), v.end(), pos) == v.end()) v.push_back(pos);
    };
    int clusters = 0;
    w.children(seg.data, seg.end, [&](const Elem& c) {
        switch (c.id) {
            case kSeekHead: seekHeads.push_back(c.start); break;
            case kInfo: case kTracks: case kTags: case kAttachments: note(c.id, c.start); break;
            case kCluster:
                // Past here is audio; skip it only while there is no SeekHead to follow.
                if (c.unknownSize || !seekHeads.empty() || ++clusters > kMaxClustersWithoutSeek) return false;
                break;
            default: break;
        }
        return true;
    });
    for (size_t i = 0; i < seekHeads.size() && i < 4; ++i) {
        Elem sh;
        if (!w.header(seekHeads[i], seg.end, sh) || sh.id != kSeekHead) continue;
        w.children(sh.data, sh.end, [&](const Elem& seek) {
            if (seek.id != kSeek) return true;
            uint32_t id = 0;
            uint64_t pos = 0;
            bool hasPos = false;
            w.children(seek.data, seek.end, [&](const Elem& s) {
                if (s.id == kSeekId) {
                    const std::vector<uint8_t> b = w.bytes(s, 4);
                    id = 0;
                    for (uint8_t x : b) id = (id << 8) | x;
                } else if (s.id == kSeekPos) {
                    pos = w.uintValue(s);
                    hasPos = true;
                }
                return true;
            });
            if (!hasPos || pos >= seg.end - seg.data) return true;
            const uint64_t abs = seg.data + pos;
            if (id == kSeekHead) {
                if (std::find(seekHeads.begin(), seekHeads.end(), abs) == seekHeads.end()) seekHeads.push_back(abs);
            } else if (id == kInfo || id == kTracks || id == kTags || id == kAttachments) {
                note(id, abs);
            }
            return true;
        });
    }

    // Each wanted element, read where it was found; a pointer to anything else is ignored.
    auto visit = [&](uint32_t id, auto&& read) {
        for (uint64_t pos : at[id]) {
            Elem x;
            if (w.header(pos, seg.end, x) && x.id == id && !x.unknownSize) read(x);
        }
    };
    visit(kInfo, [&](const Elem& x) { readInfo(w, x, p); });
    visit(kTracks, [&](const Elem& x) { if (!p.audio) readTracks(w, x, p); });
    visit(kTags, [&](const Elem& x) { readTags(w, x, p); });
    visit(kAttachments, [&](const Elem& x) { readAttachments(w, x, t); });

    applyTags(p, t);
    const double seconds = p.duration * static_cast<double>(p.timecodeScale) / 1e9;
    if (std::isfinite(seconds) && seconds > 0 && seconds < 1e9) t.duration = seconds;
    if (p.audio) {
        t.codec = codecName(p.codecId);
        if (std::isfinite(p.sampleRate) && p.sampleRate > 0 && p.sampleRate < 1e7)
            t.sampleRate = static_cast<int>(std::lround(p.sampleRate));
        t.channels = p.channels <= 1024 ? static_cast<int>(p.channels) : 0;
        auto bps = p.bpsByTrack.find(p.audioUid);
        if (p.audioUid && bps != p.bpsByTrack.end()) {
            int n = 0, of = 0;
            numberPair(bps->second, n, of);
            t.bitrate = n;
        }
    }
    return true;
}

} // namespace broaudio::tags
