// Ogg Vorbis and Ogg Opus: the identification and comment packets at the
// start of the first logical stream, and the length from the last page's
// granule position. Pages are read one at a time, so only the headers and
// the file's tail are read however long it is; a comment packet may run
// over many pages (a big picture) and is put back together.

#include "tags_internal.h"

#include <algorithm>

namespace broaudio::tags {

namespace {

constexpr size_t kTail = 128 * 1024;    // a page is at most 65307 bytes

struct Page {
    uint32_t serial = 0;
    uint8_t lacing[255];
    int segments = 0;
    uint64_t bodyAt = 0;
    uint64_t next = 0;
};

bool readPage(ByteSource& src, uint64_t o, Page& p)
{
    uint8_t h[27];
    if (src.read(o, h, 27) < 27 || !isAscii(h, 27, 0, "OggS")) return false;
    p.serial = le32(h + 14);
    p.segments = h[26];
    if (src.read(o + 27, p.lacing, static_cast<size_t>(p.segments)) < static_cast<size_t>(p.segments)) return false;
    uint64_t body = 0;
    for (int i = 0; i < p.segments; ++i) body += p.lacing[i];
    p.bodyAt = o + 27 + static_cast<uint64_t>(p.segments);
    p.next = p.bodyAt + body;
    return true;
}

// The first two packets of the first logical stream (identification, comments).
std::vector<std::vector<uint8_t>> headerPackets(ByteSource& src, uint32_t& serial)
{
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> cur;
    uint64_t o = 0;
    bool haveSerial = false;
    Page p;
    while (packets.size() < 2 && o < src.size()) {
        if (!readPage(src, o, p)) break;
        if (!haveSerial) { serial = p.serial; haveSerial = true; }
        if (p.serial == serial) {
            uint64_t at = p.bodyAt;
            uint64_t seg = 0;
            for (int i = 0; i < p.segments; ++i) {
                seg += p.lacing[i];
                if (p.lacing[i] < 255) {
                    // A packet ends here: what was carried over plus this run.
                    const std::vector<uint8_t> run = src.read(at, static_cast<size_t>(seg));
                    cur.insert(cur.end(), run.begin(), run.end());
                    at += seg;
                    seg = 0;
                    packets.push_back(std::move(cur));
                    cur.clear();
                    if (packets.size() == 2) break;
                }
            }
            if (packets.size() < 2 && seg) {
                const std::vector<uint8_t> run = src.read(at, static_cast<size_t>(seg));
                cur.insert(cur.end(), run.begin(), run.end());
            }
            if (cur.size() > kMaxBlock) break;
        }
        o = p.next;
    }
    return packets;
}

// The granule position of the stream's last page, read backwards from the end.
uint64_t lastGranule(ByteSource& src, uint32_t serial)
{
    const uint64_t start = src.size() > kTail ? src.size() - kTail : 0;
    const std::vector<uint8_t> b = src.read(start, static_cast<size_t>(src.size() - start));
    if (b.size() < 27) return 0;
    for (size_t i = b.size() - 27 + 1; i-- > 0;) {
        if (b[i] != 'O' || !isAscii(b.data(), b.size(), i, "OggS")) continue;
        if (le32(b.data() + i + 14) != serial) continue;
        const uint64_t g = le64(b.data() + i + 6);
        if (g > 0 && g < (uint64_t(1) << 52)) return g;
    }
    return 0;
}

} // namespace

bool readOgg(ByteSource& src, AudioTags& t)
{
    uint8_t magic[4];
    if (src.read(0, magic, 4) < 4 || !isAscii(magic, 4, 0, "OggS")) return false;
    uint32_t serial = 0;
    const auto packets = headerPackets(src, serial);
    if (packets.empty()) return false;
    t.container = "ogg";
    const std::vector<uint8_t>& id = packets[0];
    uint32_t preskip = 0;
    if (id.size() >= 16 && id[0] == 1 && isAscii(id.data(), id.size(), 1, "vorbis")) {
        t.codec = "vorbis";
        t.channels = id[11];
        t.sampleRate = static_cast<int>(le32(id.data() + 12));
    } else if (id.size() >= 19 && isAscii(id.data(), id.size(), 0, "OpusHead")) {
        t.codec = "opus";
        t.channels = id[9];
        t.sampleRate = 48000;                                     // Opus granules always count 48 kHz
        preskip = le16(id.data() + 10);
    } else if (id.size() >= 5 && id[0] == 0x7f && isAscii(id.data(), id.size(), 1, "FLAC")) {
        t.codec = "flac";
        return true;
    } else {
        return true;                                              // an Ogg, but not a codec we read
    }
    if (packets.size() >= 2) {
        const std::vector<uint8_t>& c = packets[1];
        if (t.codec == "vorbis" && c.size() >= 7 && c[0] == 3 && isAscii(c.data(), c.size(), 1, "vorbis"))
            readVorbisComments(c.data() + 7, c.size() - 7, t);
        else if (t.codec == "opus" && isAscii(c.data(), c.size(), 0, "OpusTags"))
            readVorbisComments(c.data() + 8, c.size() - 8, t);
    }
    const uint64_t g = lastGranule(src, serial);
    if (g && t.sampleRate) {
        t.duration = double(g > preskip ? g - preskip : 0) / t.sampleRate;
        if (t.duration > 0) t.bitrate = static_cast<int>(double(src.size()) * 8 / t.duration + 0.5);
    }
    return true;
}

} // namespace broaudio::tags
