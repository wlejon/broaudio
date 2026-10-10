// The AudioSpecificConfig parser every AAC backend shares, and the
// no-decoder stub for platforms without one.

#include "aac_decoder.h"

namespace broaudio {

namespace {

struct Bits {
    const uint8_t* b;
    size_t n;
    size_t pos = 0;
    bool ok = true;
    uint32_t get(int count)
    {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            if (pos >= n * 8) { ok = false; return 0; }
            v = (v << 1) | ((b[pos >> 3] >> (7 - (pos & 7))) & 1);
            ++pos;
        }
        return v;
    }
};

int objectType(Bits& r)
{
    int t = static_cast<int>(r.get(5));
    if (t == 31) t = 32 + static_cast<int>(r.get(6));
    return t;
}

int sampleRate(Bits& r)
{
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350};
    const uint32_t i = r.get(4);
    if (i == 15) return static_cast<int>(r.get(24));
    return i < 13 ? kRates[i] : 0;
}

} // namespace

bool parseAudioSpecificConfig(const uint8_t* asc, size_t size, AacConfig& out)
{
    out = AacConfig{};
    if (!asc || size < 2) return false;
    Bits r{asc, size};
    out.objectType = objectType(r);
    out.sampleRate = sampleRate(r);
    out.channels = static_cast<int>(r.get(4));
    if (out.objectType == 5 || out.objectType == 29) {
        out.extSampleRate = sampleRate(r);
        const int core = objectType(r);
        (void)core;  // the core is AAC-LC in practice; the object type stays 5/29
    }
    return r.ok && out.sampleRate > 0;
}

#if !(defined(BROAUDIO_HAS_AAC) && BROAUDIO_HAS_AAC)
std::unique_ptr<AacDecoder> makePlatformAacDecoder(const uint8_t*, size_t, std::string* error)
{
    if (error)
        *error = "M4A/AAC needs a platform AAC decoder (Media Foundation on Windows, "
                 "AudioToolbox on macOS); this build has none";
    return nullptr;
}
#endif

} // namespace broaudio
