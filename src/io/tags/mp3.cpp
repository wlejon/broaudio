// MP3: the ID3v2 tag in front, ID3v1 at the back filling what v2 left empty,
// and the length from the first frame: a Xing/Info or VBRI header's frame
// count when there is one; else the bit rate over the audio's size when the
// first frames share one bit rate (CBR); else a walk over the frame headers
// (a few bytes per frame, no decoding).

#include "tags_internal.h"

namespace broaudio::tags {

namespace {

struct FrameHeader {
    int version = 0;      // 1, 2, or 25 (MPEG 2.5)
    int layer = 0;
    int kbps = 0;
    int sampleRate = 0;
    bool mono = false;
    int samples = 0;
    int length = 0;
};

bool frameHeader(const uint8_t* b, size_t n, size_t o, FrameHeader& h)
{
    if (o + 4 > n || b[o] != 0xff || (b[o + 1] & 0xe0) != 0xe0) return false;
    static const int kRates[3][15] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},     // V1 L1
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},        // V1 L2
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},         // V1 L3
    };
    static const int kRates2[2][15] = {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},        // V2 L1
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},             // V2 L2/L3
    };
    static const int kSampleRates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    const int v = (b[o + 1] >> 3) & 3;
    const int layerBits = (b[o + 1] >> 1) & 3;
    const int bi = b[o + 2] >> 4;
    const int si = (b[o + 2] >> 2) & 3;
    if (v == 1 || layerBits == 0 || bi == 0 || bi == 15 || si == 3) return false;
    h.version = v == 3 ? 1 : v == 2 ? 2 : 25;
    h.layer = 4 - layerBits;
    h.kbps = h.version == 1 ? kRates[h.layer - 1][bi] : kRates2[h.layer == 1 ? 0 : 1][bi];
    h.sampleRate = kSampleRates[h.version == 1 ? 0 : h.version == 2 ? 1 : 2][si];
    const int pad = (b[o + 2] >> 1) & 1;
    h.mono = (b[o + 3] >> 6) == 3;
    h.samples = h.layer == 1 ? 384 : (h.layer == 2 || h.version == 1) ? 1152 : 576;
    h.length = h.layer == 1 ? ((12 * h.kbps * 1000) / h.sampleRate + pad) * 4
                            : (h.samples / 8) * h.kbps * 1000 / h.sampleRate + pad;
    return true;
}

} // namespace

bool readMp3(ByteSource& src, AudioTags& t)
{
    const uint64_t start = readId3v2(src, 0, t);

    // The first real frame: two in a row when the second fits in the window.
    constexpr size_t kWindow = 128 * 1024;
    const std::vector<uint8_t> b = src.read(start, kWindow);
    FrameHeader h;
    size_t at = 0;
    bool found = false;
    for (size_t i = 0; i + 4 <= b.size(); ++i) {
        if (b[i] != 0xff) continue;
        if (!frameHeader(b.data(), b.size(), i, h) || h.length < 24) continue;
        FrameHeader next;
        const size_t ni = i + static_cast<size_t>(h.length);
        if (ni + 4 <= b.size() && !frameHeader(b.data(), b.size(), ni, next)) continue;
        at = i;
        found = true;
        break;
    }
    if (!start && !found) return false;

    t.container = "mp3";
    AudioTags v1;
    const bool hasV1 = readId3v1(src, v1);
    if (hasV1) fillEmpty(t, v1);
    if (!found) {
        t.codec = "mp3";
        return true;
    }

    const uint64_t frameAt = start + at;
    t.codec = h.layer == 3 ? "mp3" : h.layer == 2 ? "mp2" : "mp1";
    t.sampleRate = h.sampleRate;
    t.channels = h.mono ? 1 : 2;
    const uint8_t* f = b.data() + at;
    const size_t fn = b.size() - at;
    const uint64_t end = src.size() - (hasV1 ? 128 : 0);
    const uint64_t audio = end > frameAt ? end - frameAt : 0;

    // Xing / Info (LAME) after the side information.
    const size_t side = h.version == 1 ? (h.mono ? 17 : 32) : (h.mono ? 9 : 17);
    const size_t x = 4 + side;
    if (isAscii(f, fn, x, "Xing") || isAscii(f, fn, x, "Info")) {
        if (x + 16 <= fn) {
            const uint32_t flags = be32(f + x + 4);
            size_t p = x + 8;
            uint32_t frames = 0, bytes = 0;
            if (flags & 1) { frames = be32(f + p); p += 4; }
            if ((flags & 2) && p + 4 <= fn) bytes = be32(f + p);
            if (frames) {
                t.duration = double(frames) * h.samples / h.sampleRate;
                const double b2 = bytes ? double(bytes) : double(audio);
                if (t.duration > 0) t.bitrate = static_cast<int>(b2 * 8 / t.duration + 0.5);
                return true;
            }
        }
    }
    // VBRI (Fraunhofer) at a fixed 32 bytes after the header.
    if (isAscii(f, fn, 36, "VBRI") && 36 + 18 <= fn) {
        const uint32_t bytes = be32(f + 36 + 10);
        const uint32_t frames = be32(f + 36 + 14);
        if (frames) {
            t.duration = double(frames) * h.samples / h.sampleRate;
            if (t.duration > 0) t.bitrate = static_cast<int>(double(bytes ? bytes : audio) * 8 / t.duration + 0.5);
            return true;
        }
    }

    // CBR: the first frames share one bit rate, so the size gives the length.
    bool cbr = true;
    {
        size_t o = at;
        for (int k = 0; k < 16; ++k) {
            FrameHeader g;
            if (!frameHeader(b.data(), b.size(), o, g)) break;     // out of the window: trust what was seen
            if (g.kbps != h.kbps) { cbr = false; break; }
            o += static_cast<size_t>(g.length);
        }
    }
    if (cbr) {
        t.bitrate = h.kbps * 1000;
        t.duration = double(audio) * 8.0 / t.bitrate;
        return true;
    }

    // VBR with no header: walk the frame headers, a 4-byte read per frame.
    uint64_t o = frameAt, samples = 0, bytes = 0;
    int lost = 0;
    while (o + 4 <= end) {
        uint8_t hb[4];
        if (src.read(o, hb, 4) < 4) break;
        FrameHeader g;
        if (!frameHeader(hb, 4, 0, g) || g.length < 24 || g.sampleRate != h.sampleRate) {
            // Lost sync (junk between frames): look for the next header nearby.
            if (++lost > 64) break;
            const std::vector<uint8_t> w = src.read(o + 1, 4096);
            size_t k = 0;
            for (; k + 4 <= w.size(); ++k)
                if (w[k] == 0xff && frameHeader(w.data(), w.size(), k, g) && g.sampleRate == h.sampleRate) break;
            if (k + 4 > w.size()) break;
            o += 1 + k;
            continue;
        }
        samples += static_cast<uint64_t>(g.samples);
        bytes += static_cast<uint64_t>(g.length);
        o += static_cast<uint64_t>(g.length);
    }
    if (samples) {
        t.duration = double(samples) / h.sampleRate;
        t.bitrate = static_cast<int>(double(bytes) * 8 / t.duration + 0.5);
    }
    return true;
}

} // namespace broaudio::tags
