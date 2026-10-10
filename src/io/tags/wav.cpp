// WAV: the length from the fmt and data chunks, and tags from a LIST/INFO
// chunk (INAM title, IART artist, IPRD album, ITRK / IPRT track, ICRD date,
// IGNR genre) or an "id3 " chunk, wherever in the file they are.

#include "tags_internal.h"

#include <algorithm>

namespace broaudio::tags {

namespace {

// INFO text: UTF-8 when it is valid UTF-8, else Latin-1.
std::string infoText(const uint8_t* b, size_t n)
{
    while (n && b[n - 1] == 0) --n;
    return trim(isValidUtf8(b, n) ? utf8Clean(b, n) : latin1ToUtf8(b, n));
}

} // namespace

bool readWav(ByteSource& src, AudioTags& t)
{
    uint8_t head[12];
    if (src.read(0, head, 12) < 12 || !isAscii(head, 12, 0, "RIFF") || !isAscii(head, 12, 8, "WAVE"))
        return false;
    t.container = "wav";
    t.codec = "pcm";
    uint32_t byteRate = 0;
    uint64_t dataLen = 0;
    AudioTags id3;
    uint64_t o = 12;
    for (int guard = 0; guard < 1024 && o + 8 <= src.size(); ++guard) {
        uint8_t ch[8];
        if (src.read(o, ch, 8) < 8) break;
        const std::string id(reinterpret_cast<const char*>(ch), 4);
        const uint32_t len = le32(ch + 4);
        const uint64_t body = o + 8;
        if (id == "fmt ") {
            const std::vector<uint8_t> f = src.read(body, std::min<uint32_t>(len, 40));
            if (f.size() >= 16) {
                uint32_t tag = le16(f.data());
                if (tag == 0xfffe && f.size() >= 26) tag = le16(f.data() + 24);   // WAVE_FORMAT_EXTENSIBLE
                t.channels = static_cast<int>(le16(f.data() + 2));
                t.sampleRate = static_cast<int>(le32(f.data() + 4));
                byteRate = le32(f.data() + 8);
                if (!byteRate) byteRate = static_cast<uint32_t>(t.sampleRate) * le16(f.data() + 2) * (le16(f.data() + 14) >> 3);
                t.codec = tag == 1 ? "pcm" : tag == 3 ? "float" : tag == 0x55 ? "mp3"
                        : tag == 6 ? "alaw" : tag == 7 ? "mulaw" : tag == 2 || tag == 0x11 ? "adpcm" : "wav";
            }
        } else if (id == "data") {
            dataLen = std::min<uint64_t>(len, src.size() > body ? src.size() - body : 0);
        } else if (id == "LIST" && len >= 4 && len < (1u << 20)) {
            const std::vector<uint8_t> b = src.read(body, len);
            if (isAscii(b.data(), b.size(), 0, "INFO")) {
                for (size_t i = 4; i + 8 <= b.size();) {
                    const std::string key(reinterpret_cast<const char*>(b.data() + i), 4);
                    const uint32_t n = le32(b.data() + i + 4);
                    const size_t at = i + 8;
                    const size_t take = static_cast<size_t>(std::min<uint64_t>(n, b.size() - at));
                    const std::string v = infoText(b.data() + at, take);
                    int num = 0, of = 0;
                    if (key == "INAM") t.title = v;
                    else if (key == "IART") t.artist = v;
                    else if (key == "IPRD") t.album = v;
                    else if (key == "ITRK" || key == "IPRT") { numberPair(v, num, of); t.track = num; if (of) t.trackTotal = of; }
                    else if (key == "ICRD") t.year = yearOf(v);
                    else if (key == "IGNR") t.genre = v;
                    i = at + n + (n & 1);
                }
            }
        } else if ((id == "id3 " || id == "ID3 ") && len < kMaxBlock) {
            const std::vector<uint8_t> b = src.read(body, len);
            ByteSource mem;
            mem.openMemory(b.data(), b.size());
            readId3v2(mem, 0, id3);
        }
        o = body + len + (len & 1);
    }
    fillEmpty(t, id3);
    if (byteRate) t.bitrate = static_cast<int>(std::min<uint64_t>(uint64_t(byteRate) * 8, 0x7fffffff));
    if (byteRate && dataLen) t.duration = double(dataLen) / byteRate;
    return true;
}

} // namespace broaudio::tags
