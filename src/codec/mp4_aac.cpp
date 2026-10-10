#include "mp4_aac.h"

#include "aac_decoder.h"
#include "../io/byte_source.h"
#include "../io/tags/tags_internal.h"

#include "minimp4/minimp4.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace broaudio {

bool looksLikeMp4(const uint8_t* head, size_t size)
{
    return size >= 8 && std::memcmp(head + 4, "ftyp", 4) == 0;
}

namespace {

struct Packet {
    uint64_t offset = 0;
    uint32_t size = 0;
    uint64_t time = 0;       // start, in the media timescale
};

int readCallback(int64_t offset, void* buffer, size_t size, void* token)
{
    auto* src = static_cast<ByteSource*>(token);
    if (offset < 0) return 1;
    return src->read(static_cast<uint64_t>(offset), static_cast<uint8_t*>(buffer), size) == size ? 0 : 1;
}

} // namespace

struct Mp4AacDecoder::Impl {
    ByteSource src;
    std::unique_ptr<AacDecoder> dec;
    std::vector<Packet> packets;
    uint64_t endTime = 0;      // the last packet's end, media timescale
    uint32_t timescale = 0;
    int rate = 0;
    int channels = 0;
    double scale = 1.0;        // output frames per media tick
    uint64_t skip = 0;         // priming frames dropped at the start
    uint64_t total = 0;

    size_t next = 0;           // the next packet to decode
    bool drained = false;      // the decoder was drained after the last packet
    std::vector<float> pcm;    // the last packet's output
    size_t pcmAt = 0;          // frames of it already given out (or dropped)
    uint64_t discard = 0;      // frames still to drop before the position
    uint64_t pos = 0;          // frames given out
    std::vector<uint8_t> buf;

    uint64_t toFrames(uint64_t ticks) const { return static_cast<uint64_t>(std::llround(double(ticks) * scale)); }

    bool open(std::string* error)
    {
        auto fail = [&](const std::string& why) {
            if (error) *error = why;
            return false;
        };
        tags::Mp4AudioInfo info;
        if (!tags::readMp4Info(src, info, nullptr)) return fail("corrupt MP4 file (no movie header)");
        if (!info.found) return fail("this MP4 has no audio track");
        const bool aac = info.format == 0x6d703461 /* mp4a */ &&
                         (info.objectType == 0x40 || (info.objectType >= 0x66 && info.objectType <= 0x68));
        if (!aac) {
            AudioTags t;
            tags::readMp4(src, t);
            return fail("MP4 audio is " + (t.codec.empty() ? std::string("an unknown codec") : t.codec) +
                        "; only AAC is supported in MP4/M4A");
        }

        MP4D_demux_t mp4;
        std::memset(&mp4, 0, sizeof(mp4));
        if (!MP4D_open(&mp4, readCallback, &src, static_cast<int64_t>(src.size())))
            return fail("corrupt MP4 file (unreadable sample tables)");
        int track = -1;
        for (unsigned i = 0; i < mp4.track_count; ++i)
            if (mp4.track[i].handler_type == MP4D_HANDLER_TYPE_SOUN) { track = static_cast<int>(i); break; }
        if (track < 0 || !mp4.track[track].dsi || !mp4.track[track].dsi_bytes || !mp4.track[track].sample_count) {
            MP4D_close(&mp4);
            return fail("corrupt MP4 file (no AAC configuration or no samples)");
        }
        const MP4D_track_t& tr = mp4.track[track];
        std::vector<uint8_t> asc(tr.dsi, tr.dsi + tr.dsi_bytes);
        timescale = tr.timescale ? tr.timescale : info.mediaTimescale;
        packets.reserve(tr.sample_count);
        uint64_t t = 0;
        for (unsigned s = 0; s < tr.sample_count; ++s) {
            unsigned bytes = 0, ts = 0, dur = 0;
            const MP4D_file_offset_t off = MP4D_frame_offset(&mp4, static_cast<unsigned>(track), s, &bytes, &ts, &dur);
            if (!bytes) continue;
            packets.push_back(Packet{static_cast<uint64_t>(off), bytes, t});
            t += dur ? dur : 1024;
        }
        endTime = t;
        MP4D_close(&mp4);
        if (packets.empty() || !timescale) return fail("corrupt MP4 file (empty audio track)");

        dec = makePlatformAacDecoder(asc.data(), asc.size(), error);
        if (!dec) return false;

        // The output format is certain once a packet has produced audio
        // (implicit HE-AAC signalling doubles the rate the ASC states).
        std::vector<float> probe;
        for (size_t i = 0; i < packets.size() && i < 8 && probe.empty(); ++i) {
            if (!loadPacket(i)) continue;
            dec->decode(buf.data(), buf.size(), probe);
        }
        rate = dec->sampleRate();
        channels = dec->channels();
        if (rate <= 0 || channels <= 0 || probe.empty()) return fail("the AAC decoder produced no audio");

        scale = double(rate) / timescale;
        // Output frame i of a decode from packet s is media frame
        // time(s) + i - latency; the edit list's media_time is the priming.
        const uint64_t priming = info.editMediaTime > 0 ? toFrames(static_cast<uint64_t>(info.editMediaTime)) : 0;
        skip = priming + static_cast<uint64_t>(std::max(0, dec->latency()));
        const uint64_t all = toFrames(endTime);
        const uint64_t avail = all > priming ? all - priming : 0;
        if (info.editDuration && info.movieTimescale)
            total = static_cast<uint64_t>(std::llround(double(info.editDuration) * rate / info.movieTimescale));
        else
            total = avail;
        total = std::min(total, avail);
        if (!total) return fail("this MP4's audio track is empty");
        seek(0);
        return true;
    }

    bool loadPacket(size_t i)
    {
        const Packet& p = packets[i];
        buf.resize(p.size);
        return src.read(p.offset, buf.data(), p.size) == p.size;
    }

    void seek(uint64_t frame)
    {
        const uint64_t abs = frame + skip;
        const uint64_t ticks = static_cast<uint64_t>(double(abs) / scale);
        auto it = std::upper_bound(packets.begin(), packets.end(), ticks,
                                   [](uint64_t v, const Packet& p) { return v < p.time; });
        size_t p = it == packets.begin() ? 0 : static_cast<size_t>(it - packets.begin()) - 1;
        const size_t start = p >= 2 ? p - 2 : 0;
        dec->reset();
        next = start;
        drained = false;
        pcm.clear();
        pcmAt = 0;
        const uint64_t startFrame = toFrames(packets[start].time);
        discard = abs > startFrame ? abs - startFrame : 0;
        pos = frame;
    }

    // Decode the next packet into pcm; a packet that fails is silence of its length.
    bool decodeNext()
    {
        pcm.clear();
        pcmAt = 0;
        if (next >= packets.size()) {
            // Past the last packet: what a decoder with latency still holds.
            if (drained) return false;
            drained = true;
            dec->drain(pcm);
            if (pcm.empty()) return false;
        } else {
            const size_t i = next++;
            if (!loadPacket(i) || !dec->decode(buf.data(), buf.size(), pcm) || pcm.empty()) {
                const uint64_t end = i + 1 < packets.size() ? packets[i + 1].time : endTime;
                const uint64_t frames = toFrames(end) - toFrames(packets[i].time);
                pcm.assign(static_cast<size_t>(frames) * channels, 0.0f);
            }
        }
        const size_t frames = pcm.size() / static_cast<size_t>(channels);
        const size_t drop = static_cast<size_t>(std::min<uint64_t>(discard, frames));
        pcmAt = drop;
        discard -= drop;
        return true;
    }

    int read(float* dst, int maxFrames)
    {
        int got = 0;
        const size_t ch = static_cast<size_t>(channels);
        while (got < maxFrames && pos < total) {
            const size_t frames = pcm.size() / ch;
            if (pcmAt >= frames) {
                if (!decodeNext()) break;
                continue;
            }
            const size_t n = static_cast<size_t>(std::min<uint64_t>(
                {uint64_t(frames - pcmAt), uint64_t(maxFrames - got), total - pos}));
            std::memcpy(dst + static_cast<size_t>(got) * ch, pcm.data() + pcmAt * ch, n * ch * sizeof(float));
            pcmAt += n;
            got += static_cast<int>(n);
            pos += n;
        }
        return got;
    }
};

Mp4AacDecoder::Mp4AacDecoder() : impl_(new Impl) {}
Mp4AacDecoder::~Mp4AacDecoder() = default;

bool Mp4AacDecoder::openFile(const char* path, std::string* error)
{
    if (!impl_->src.openFile(path)) {
        if (error) *error = std::string("cannot open file: ") + (path ? path : "");
        return false;
    }
    return impl_->open(error);
}

bool Mp4AacDecoder::openMemory(const uint8_t* data, size_t size, std::string* error)
{
    impl_->src.openMemory(data, size);
    return impl_->open(error);
}

int Mp4AacDecoder::channels() const { return impl_->channels; }
int Mp4AacDecoder::sampleRate() const { return impl_->rate; }
uint64_t Mp4AacDecoder::totalFrames() const { return impl_->total; }

int Mp4AacDecoder::readFrames(float* dst, int maxFrames)
{
    if (!dst || maxFrames <= 0 || !impl_->dec) return 0;
    return impl_->read(dst, maxFrames);
}

bool Mp4AacDecoder::seekToFrame(uint64_t frame)
{
    if (!impl_->dec || frame > impl_->total) return false;
    impl_->seek(frame);
    return true;
}

} // namespace broaudio
