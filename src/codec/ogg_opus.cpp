#include "ogg_opus.h"

#include <opus_multistream.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace broaudio {

namespace {

// ---------------------------------------------------------------------------
// Random-access byte sources
// ---------------------------------------------------------------------------

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Copy up to n bytes at `off` into dst; returns bytes copied (short at EOF).
    virtual size_t readAt(uint64_t off, uint8_t* dst, size_t n) = 0;
    virtual uint64_t size() const = 0;
};

class FileSource final : public ByteSource {
public:
    ~FileSource() override { if (f_) fclose(f_); }

    bool open(const char* path)
    {
        f_ = fopen(path, "rb");
        if (!f_) return false;
        if (!seek(0, SEEK_END)) return false;
        size_ = tell();
        pos_ = size_;
        return true;
    }

    size_t readAt(uint64_t off, uint8_t* dst, size_t n) override
    {
        if (off >= size_ || n == 0) return 0;
        // Sequential page reads land exactly where the last read stopped, so
        // the stdio buffer is reused instead of being thrown away by a seek.
        if (off != pos_) {
            if (!seek(static_cast<int64_t>(off), SEEK_SET)) return 0;
            pos_ = off;
        }
        size_t got = fread(dst, 1, n, f_);
        pos_ += got;
        return got;
    }

    uint64_t size() const override { return size_; }

private:
    bool seek(int64_t off, int whence)
    {
#if defined(_WIN32)
        return _fseeki64(f_, off, whence) == 0;
#else
        return fseeko(f_, static_cast<off_t>(off), whence) == 0;
#endif
    }
    uint64_t tell()
    {
#if defined(_WIN32)
        int64_t t = _ftelli64(f_);
#else
        int64_t t = static_cast<int64_t>(ftello(f_));
#endif
        return t < 0 ? 0 : static_cast<uint64_t>(t);
    }

    FILE* f_ = nullptr;
    uint64_t size_ = 0;
    uint64_t pos_ = 0;
};

class MemorySource final : public ByteSource {
public:
    MemorySource(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    size_t readAt(uint64_t off, uint8_t* dst, size_t n) override
    {
        if (off >= n_) return 0;
        size_t c = std::min<uint64_t>(n, n_ - off);
        std::memcpy(dst, d_ + off, c);
        return c;
    }
    uint64_t size() const override { return n_; }

private:
    const uint8_t* d_;
    size_t n_;
};

// ---------------------------------------------------------------------------
// Ogg pages (RFC 3533)
// ---------------------------------------------------------------------------

// Ogg's CRC-32: polynomial 0x04c11db7, unreflected, zero init, no final xor.
constexpr std::array<uint32_t, 256> makeCrcTable()
{
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; ++k)
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
        t[i] = r;
    }
    return t;
}
constexpr std::array<uint32_t, 256> kCrc = makeCrcTable();

uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        crc = (crc << 8) ^ kCrc[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

constexpr uint8_t kContinued = 0x01;
constexpr uint8_t kEos = 0x04;

struct Page {
    uint64_t offset = 0;
    uint32_t headerLen = 0;
    uint32_t bodyLen = 0;
    uint8_t flags = 0;
    int64_t granule = -1;
    uint32_t serial = 0;
    uint8_t nseg = 0;
    uint8_t lacing[255] = {};
    std::vector<uint8_t> body;

    uint64_t end() const { return offset + headerLen + bodyLen; }
    bool endsPacket() const
    {
        for (int i = 0; i < nseg; ++i)
            if (lacing[i] < 255) return true;
        return false;
    }
};

// Read and CRC-check the page starting exactly at `off`.
bool readPageAt(ByteSource& src, uint64_t off, Page& pg)
{
    uint8_t h[27 + 255];
    if (src.readAt(off, h, 27) != 27) return false;
    if (std::memcmp(h, "OggS", 4) != 0 || h[4] != 0) return false;
    const uint8_t nseg = h[26];
    if (nseg && src.readAt(off + 27, h + 27, nseg) != nseg) return false;

    uint32_t bodyLen = 0;
    for (int i = 0; i < nseg; ++i) bodyLen += h[27 + i];
    pg.body.resize(bodyLen);
    if (bodyLen && src.readAt(off + 27 + nseg, pg.body.data(), bodyLen) != bodyLen)
        return false;

    const uint32_t stored = le32(h + 22);
    h[22] = h[23] = h[24] = h[25] = 0;
    uint32_t crc = crcUpdate(0, h, 27u + nseg);
    crc = crcUpdate(crc, pg.body.data(), bodyLen);
    if (crc != stored) return false;

    pg.offset = off;
    pg.headerLen = 27u + nseg;
    pg.bodyLen = bodyLen;
    pg.flags = h[5];
    uint64_t g = 0;
    for (int i = 7; i >= 0; --i) g = (g << 8) | h[6 + i];
    pg.granule = static_cast<int64_t>(g);
    pg.serial = le32(h + 14);
    pg.nseg = nseg;
    std::memcpy(pg.lacing, h + 27, nseg);
    return true;
}

// First valid page of `serial` starting in [from, limit), optionally one that
// completes a packet (granule != -1). Scans for the capture pattern, so it
// resynchronises from an arbitrary offset (bisection, corrupt data).
bool findPage(ByteSource& src, uint64_t from, uint64_t limit, uint32_t serial,
              bool needGranule, Page& out)
{
    constexpr size_t kChunk = 65536;
    std::vector<uint8_t> buf(kChunk);
    uint64_t p = from;
    while (p < limit) {
        size_t n = src.readAt(p, buf.data(), kChunk);
        if (n < 4) return false;
        bool jumped = false;
        for (size_t i = 0; i + 4 <= n; ++i) {
            if (buf[i] != 'O' || std::memcmp(buf.data() + i, "OggS", 4) != 0) continue;
            const uint64_t cand = p + i;
            if (cand >= limit) return false;
            if (!readPageAt(src, cand, out)) continue;
            if (out.serial == serial && (!needGranule || out.granule != -1)) return true;
            // A valid page we do not want: skip over it whole.
            p = out.end();
            jumped = true;
            break;
        }
        if (!jumped) p += n - 3;
    }
    return false;
}

constexpr int kMaxPacketFrames = 5760;   // 120 ms at 48 kHz, Opus's longest
constexpr int64_t kPreRoll = 3840;       // 80 ms (RFC 7845 section 4.6)
// Below this many bytes a seek stops bisecting and walks pages forward: a
// few typical pages (a page is ~1 s of audio from most muxers, 4-8 KB).
constexpr uint64_t kLinearSpan = 16384;

} // namespace

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct OggOpusDecoder::Impl {
    std::unique_ptr<ByteSource> src;
    OpusMSDecoder* dec = nullptr;
    int channels = 0;
    int preskip = 0;
    uint32_t serial = 0;

    uint64_t dataStart = 0;     // offset of the first audio page
    int64_t startGranule = 0;   // granule at the start of the first audio packet
    int64_t endGranule = std::numeric_limits<int64_t>::max();
    int64_t firstPageGranule = 0;

    // Packet reader.
    uint64_t pos = 0;           // offset of the next page to read
    Page page;
    bool havePage = false;
    int seg = 0;
    size_t bodyOff = 0;
    std::vector<uint8_t> partial;
    bool skipContinuation = false;
    bool dropCompleted = false; // after a seek: drop packets completing on this page
    bool eos = false;

    // Decode state.
    int64_t granule = 0;        // granule at the start of the next packet
    int64_t discardUntil = 0;   // output nothing before this granule
    std::vector<float> pcm;     // last decoded packet (kMaxPacketFrames * channels)
    int pcmBegin = 0, pcmEnd = 0;
    std::vector<uint8_t> pkt;

    ~Impl() { if (dec) opus_multistream_decoder_destroy(dec); }

    int64_t base() const { return startGranule + preskip; }

    bool nextOwnPage(Page& out)
    {
        const uint64_t size = src->size();
        while (pos < size) {
            if (readPageAt(*src, pos, out)) {
                if (out.serial == serial) return true;
                pos = out.end();
                continue;
            }
            return findPage(*src, pos + 1, size, serial, false, out);
        }
        return false;
    }

    bool nextPacket(std::vector<uint8_t>& out)
    {
        for (;;) {
            if (!havePage) {
                if (eos || !nextOwnPage(page)) { eos = true; return false; }
                havePage = true;
                seg = 0;
                bodyOff = 0;
                if (page.flags & kContinued) {
                    // A tail whose head we never saw (first page after a
                    // resync): drop it rather than decode half a packet.
                    if (partial.empty()) skipContinuation = true;
                } else {
                    partial.clear();
                }
            }
            while (seg < page.nseg) {
                const uint8_t l = page.lacing[seg++];
                if (!skipContinuation)
                    partial.insert(partial.end(), page.body.begin() + bodyOff,
                                   page.body.begin() + bodyOff + l);
                bodyOff += l;
                if (l == 255) continue;
                const bool drop = skipContinuation || dropCompleted;
                skipContinuation = false;
                if (drop || partial.empty()) { partial.clear(); continue; }
                out.swap(partial);
                partial.clear();
                return true;
            }
            havePage = false;
            dropCompleted = false;
            pos = page.end();
            if (page.flags & kEos) eos = true;
        }
    }

    // Decode the next packet into pcm[pcmBegin, pcmEnd) (trimmed). Returns
    // false at the end of the stream.
    bool decodeNext()
    {
        pcmBegin = pcmEnd = 0;
        if (granule >= endGranule) return false;
        if (!nextPacket(pkt)) return false;

        int n = opus_multistream_decode_float(dec, pkt.data(), static_cast<opus_int32>(pkt.size()),
                                              pcm.data(), kMaxPacketFrames, 0);
        if (n < 0) {
            // Corrupt packet: keep the timeline by emitting its length in
            // silence when the TOC still says how long it was.
            int ns = opus_packet_get_nb_samples(pkt.data(), static_cast<opus_int32>(pkt.size()), 48000);
            if (ns <= 0) return true;
            n = std::min(ns, kMaxPacketFrames);
            std::fill(pcm.begin(), pcm.begin() + static_cast<size_t>(n) * channels, 0.0f);
        }
        const int64_t g0 = granule;
        granule += n;
        const int64_t lo = std::max(g0, std::max(base(), discardUntil));
        const int64_t hi = std::min(g0 + n, endGranule);
        if (hi > lo) {
            pcmBegin = static_cast<int>(lo - g0);
            pcmEnd = static_cast<int>(hi - g0);
        }
        return true;
    }

    void resetReader(uint64_t at, int64_t atGranule)
    {
        opus_multistream_decoder_ctl(dec, OPUS_RESET_STATE);
        pos = at;
        havePage = false;
        partial.clear();
        skipContinuation = false;
        dropCompleted = false;
        eos = false;
        granule = atGranule;
        pcmBegin = pcmEnd = 0;
    }

    bool open(std::string* error)
    {
        auto fail = [&](const char* msg) {
            if (error) *error = msg;
            return false;
        };

        Page head;
        if (!readPageAt(*src, 0, head) || head.nseg == 0)
            return fail("corrupt Ogg Opus file (no valid first page)");
        if (head.bodyLen < 19 || std::memcmp(head.body.data(), "OpusHead", 8) != 0)
            return fail("not an Ogg Opus file (no OpusHead)");
        const uint8_t* h = head.body.data();
        if ((h[8] & 0xF0) != 0)
            return fail("unsupported Ogg Opus version");
        channels = h[9];
        preskip = h[10] | (h[11] << 8);
        const int16_t gainQ8 = static_cast<int16_t>(h[16] | (h[17] << 8));
        const int family = h[18];
        if (channels < 1) return fail("corrupt Ogg Opus file (no channels)");

        int streams = 1, coupled = channels - 1;
        unsigned char mapping[255] = {0, 1};
        if (family == 0) {
            if (channels > 2) return fail("corrupt Ogg Opus file (family 0 with more than 2 channels)");
        } else {
            if (head.bodyLen < 21u + static_cast<uint32_t>(channels))
                return fail("corrupt Ogg Opus file (short channel mapping table)");
            streams = h[19];
            coupled = h[20];
            if (streams < 1 || coupled > streams || streams + coupled > 255)
                return fail("corrupt Ogg Opus file (bad stream counts)");
            std::memcpy(mapping, h + 21, static_cast<size_t>(channels));
        }
        int err = OPUS_OK;
        dec = opus_multistream_decoder_create(48000, channels, streams, coupled, mapping, &err);
        if (!dec || err != OPUS_OK) return fail("unsupported Ogg Opus channel mapping");
        if (gainQ8 != 0) opus_multistream_decoder_ctl(dec, OPUS_SET_GAIN(gainQ8));
        serial = head.serial;

        // OpusTags may span pages; the first audio packet starts on a fresh
        // page right after the page that completes it.
        Page pg;
        pos = head.end();
        for (;;) {
            if (!nextOwnPage(pg)) return fail("corrupt Ogg Opus file (no OpusTags)");
            pos = pg.end();
            if (pg.endsPacket()) break;
        }
        dataStart = pos;

        // Start granule: the first audio page's granule minus the length of
        // the packets that complete on it (RFC 7845 section 4.5).
        if (nextOwnPage(pg) && pg.granule != -1) {
            firstPageGranule = pg.granule;
            int64_t sum = 0;
            size_t off = 0;
            size_t pktStart = 0;
            for (int i = 0; i < pg.nseg; ++i) {
                off += pg.lacing[i];
                if (pg.lacing[i] == 255) continue;
                if (off > pktStart) {
                    int ns = opus_packet_get_nb_samples(pg.body.data() + pktStart,
                                                        static_cast<opus_int32>(off - pktStart), 48000);
                    if (ns > 0) sum += ns;
                }
                pktStart = off;
            }
            startGranule = std::max<int64_t>(0, pg.granule - sum);
        }

        // End granule: the last page of ours that completes a packet. Scan
        // back from the end of the file in growing windows.
        const uint64_t size = src->size();
        int64_t last = -1;
        for (uint64_t window = 65536; ; window *= 4) {
            const uint64_t from = size > dataStart + window ? size - window : dataStart;
            uint64_t p = from;
            while (findPage(*src, p, size, serial, true, pg)) {
                last = pg.granule;
                p = pg.end();
            }
            if (last != -1 || from == dataStart) break;
        }
        if (last != -1) endGranule = last;
        if (endGranule <= base()) return fail("Ogg Opus file has no audio");

        pcm.assign(static_cast<size_t>(kMaxPacketFrames) * channels, 0.0f);
        resetReader(dataStart, startGranule);
        discardUntil = 0;
        return true;
    }

    bool seek(uint64_t frame)
    {
        if (!dec) return false;
        int64_t target = base() + static_cast<int64_t>(std::min<uint64_t>(
                                      frame, static_cast<uint64_t>(endGranule - base())));
        discardUntil = target;
        const int64_t goal = target - kPreRoll;
        if (goal < firstPageGranule) {
            resetReader(dataStart, startGranule);
            return true;
        }

        // Bisect for the last page whose granule is <= goal, then walk
        // forward over the final stretch.
        const uint64_t size = src->size();
        uint64_t lo = dataStart, hi = size;
        uint64_t bestOff = 0;
        int64_t bestGranule = -1;
        Page pg;
        while (hi > lo && hi - lo > kLinearSpan) {
            const uint64_t mid = lo + (hi - lo) / 2;
            if (!findPage(*src, mid, hi, serial, true, pg)) { hi = mid; continue; }
            if (pg.granule <= goal) {
                bestOff = pg.offset;
                bestGranule = pg.granule;
                lo = pg.end();
            } else {
                hi = mid;
            }
        }
        uint64_t p = lo;
        while (findPage(*src, p, size, serial, true, pg) && pg.granule <= goal) {
            bestOff = pg.offset;
            bestGranule = pg.granule;
            p = pg.end();
        }
        if (bestGranule < 0) {
            resetReader(dataStart, startGranule);
            return true;
        }
        // Start AT that page and drop the packets completing on it: the
        // packet that begins after them starts exactly at its granule.
        resetReader(bestOff, bestGranule);
        dropCompleted = true;
        return true;
    }
};

OggOpusDecoder::OggOpusDecoder() : impl_(new Impl) {}
OggOpusDecoder::~OggOpusDecoder() = default;

bool OggOpusDecoder::openFile(const char* path, std::string* error)
{
    impl_ = std::make_unique<Impl>();
    auto fs = std::make_unique<FileSource>();
    if (!path || !fs->open(path)) {
        if (error) *error = std::string("cannot open file: ") + (path ? path : "(null)");
        return false;
    }
    impl_->src = std::move(fs);
    return impl_->open(error);
}

bool OggOpusDecoder::openMemory(const uint8_t* data, size_t size, std::string* error)
{
    impl_ = std::make_unique<Impl>();
    impl_->src = std::make_unique<MemorySource>(data, size);
    return impl_->open(error);
}

int OggOpusDecoder::channels() const { return impl_->channels; }

uint64_t OggOpusDecoder::totalFrames() const
{
    if (!impl_->dec) return 0;
    const int64_t n = impl_->endGranule - impl_->base();
    return n > 0 ? static_cast<uint64_t>(n) : 0;
}

int OggOpusDecoder::readFrames(float* dst, int maxFrames)
{
    Impl& m = *impl_;
    if (!m.dec || !dst || maxFrames <= 0) return 0;
    int done = 0;
    while (done < maxFrames) {
        if (m.pcmBegin < m.pcmEnd) {
            const int n = std::min(maxFrames - done, m.pcmEnd - m.pcmBegin);
            std::memcpy(dst + static_cast<size_t>(done) * m.channels,
                        m.pcm.data() + static_cast<size_t>(m.pcmBegin) * m.channels,
                        sizeof(float) * static_cast<size_t>(n) * m.channels);
            m.pcmBegin += n;
            done += n;
            continue;
        }
        if (!m.decodeNext()) break;
    }
    return done;
}

bool OggOpusDecoder::seekToFrame(uint64_t frame) { return impl_->seek(frame); }

} // namespace broaudio
