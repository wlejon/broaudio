// AAC through AudioToolbox's AudioConverter (macOS): one raw access unit per
// AudioConverterFillComplexBuffer call, interleaved float32 out. The
// converter takes its AudioSpecificConfig as a magic cookie in the form an
// M4A's esds carries it (an ES_Descriptor around a DecoderConfigDescriptor
// around the ASC), so that is built here.

#include "aac_decoder.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstring>

namespace broaudio {

namespace {

// A descriptor: tag, length in four 7-bit bytes, payload.
void putDescriptor(std::vector<uint8_t>& out, uint8_t tag, const std::vector<uint8_t>& body)
{
    out.push_back(tag);
    const uint32_t n = static_cast<uint32_t>(body.size());
    out.push_back(static_cast<uint8_t>(0x80 | ((n >> 21) & 0x7f)));
    out.push_back(static_cast<uint8_t>(0x80 | ((n >> 14) & 0x7f)));
    out.push_back(static_cast<uint8_t>(0x80 | ((n >> 7) & 0x7f)));
    out.push_back(static_cast<uint8_t>(n & 0x7f));
    out.insert(out.end(), body.begin(), body.end());
}

std::vector<uint8_t> esdsCookie(const uint8_t* asc, size_t ascSize)
{
    std::vector<uint8_t> dsi(asc, asc + ascSize);
    std::vector<uint8_t> dcd = {0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // MPEG-4 audio, audio stream
    putDescriptor(dcd, 0x05, dsi);
    std::vector<uint8_t> es = {0, 0, 0};                                         // ES_ID, flags
    putDescriptor(es, 0x04, dcd);
    putDescriptor(es, 0x06, std::vector<uint8_t>{0x02});                         // SLConfig: MP4
    std::vector<uint8_t> out;
    putDescriptor(out, 0x03, es);
    return out;
}

constexpr OSStatus kNoMoreInput = 'nmor';

struct Feed {
    const uint8_t* data = nullptr;
    UInt32 size = 0;
    bool given = false;
    AudioStreamPacketDescription desc{};
};

OSStatus supply(AudioConverterRef, UInt32* ioPackets, AudioBufferList* io,
                AudioStreamPacketDescription** outDesc, void* user)
{
    auto* f = static_cast<Feed*>(user);
    if (f->given || !f->data) {
        *ioPackets = 0;
        return kNoMoreInput;
    }
    io->mNumberBuffers = 1;
    io->mBuffers[0].mData = const_cast<uint8_t*>(f->data);
    io->mBuffers[0].mDataByteSize = f->size;
    io->mBuffers[0].mNumberChannels = 0;
    f->desc.mStartOffset = 0;
    f->desc.mVariableFramesInPacket = 0;
    f->desc.mDataByteSize = f->size;
    if (outDesc) *outDesc = &f->desc;
    *ioPackets = 1;
    f->given = true;
    return noErr;
}

class AtAacDecoder final : public AacDecoder {
public:
    ~AtAacDecoder() override
    {
        if (conv_) AudioConverterDispose(conv_);
    }

    bool init(const uint8_t* asc, size_t ascSize, std::string* error)
    {
        AacConfig cfg;
        if (!parseAudioSpecificConfig(asc, ascSize, cfg)) {
            if (error) *error = "corrupt MP4 audio (unreadable AudioSpecificConfig)";
            return false;
        }
        AudioStreamBasicDescription in{};
        in.mFormatID = cfg.objectType == 29 ? kAudioFormatMPEG4AAC_HE_V2
                     : cfg.objectType == 5 ? kAudioFormatMPEG4AAC_HE : kAudioFormatMPEG4AAC;
        in.mSampleRate = cfg.sampleRate;
        in.mChannelsPerFrame = static_cast<UInt32>(cfg.channels > 0 ? cfg.channels : 2);
        in.mFramesPerPacket = 1024;

        std::vector<uint8_t> cookie = esdsCookie(asc, ascSize);
        // Let the cookie fill in what the ASC knows better (SBR rate, channels).
        UInt32 sz = sizeof(in);
        AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, static_cast<UInt32>(cookie.size()),
                               cookie.data(), &sz, &in);

        rate_ = static_cast<int>(in.mSampleRate);
        channels_ = static_cast<int>(in.mChannelsPerFrame);
        AudioStreamBasicDescription out{};
        out.mFormatID = kAudioFormatLinearPCM;
        out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        out.mSampleRate = in.mSampleRate;
        out.mChannelsPerFrame = in.mChannelsPerFrame;
        out.mBitsPerChannel = 32;
        out.mFramesPerPacket = 1;
        out.mBytesPerFrame = 4 * out.mChannelsPerFrame;
        out.mBytesPerPacket = out.mBytesPerFrame;

        in_ = in;
        out_ = out;
        cookie_ = std::move(cookie);
        framesPerPacket_ = in.mFramesPerPacket ? in.mFramesPerPacket : 1024;
        return create(error);
    }

    // A converter from the saved formats. Also how reset() starts over:
    // AudioConverterReset leaves enough of the decoder's state behind that
    // the first packets after it differ (by ~5e-3) from a fresh decode.
    bool create(std::string* error)
    {
        if (conv_) AudioConverterDispose(conv_);
        conv_ = nullptr;
        OSStatus st = AudioConverterNew(&in_, &out_, &conv_);
        if (st != noErr || !conv_) {
            if (error) *error = "AudioToolbox has no decoder for this AAC stream (OSStatus " + std::to_string(st) + ")";
            conv_ = nullptr;
            return false;
        }
        st = AudioConverterSetProperty(conv_, kAudioConverterDecompressionMagicCookie,
                                       static_cast<UInt32>(cookie_.size()), cookie_.data());
        if (st != noErr) {
            if (error) *error = "AudioToolbox refused the AAC configuration (OSStatus " + std::to_string(st) + ")";
            AudioConverterDispose(conv_);
            conv_ = nullptr;
            return false;
        }
        return true;
    }

    bool decode(const uint8_t* packet, size_t size, std::vector<float>& out) override
    {
        if (!conv_ || !packet || !size) return false;
        Feed feed;
        feed.data = packet;
        feed.size = static_cast<UInt32>(size);
        // An HE-AAC packet decodes to twice the core frames; leave room for that.
        const UInt32 room = std::max<UInt32>(framesPerPacket_, 1024) * 2;
        std::vector<float> buf(static_cast<size_t>(room) * static_cast<size_t>(channels_));
        for (int guard = 0; guard < 8; ++guard) {
            AudioBufferList list{};
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = static_cast<UInt32>(channels_);
            list.mBuffers[0].mDataByteSize = static_cast<UInt32>(buf.size() * sizeof(float));
            list.mBuffers[0].mData = buf.data();
            UInt32 frames = room;
            const OSStatus st = AudioConverterFillComplexBuffer(conv_, supply, &feed, &frames, &list, nullptr);
            if (frames) out.insert(out.end(), buf.begin(), buf.begin() + static_cast<size_t>(frames) * channels_);
            if (st == kNoMoreInput || (st == noErr && frames == 0)) return true;
            if (st != noErr) return false;
            if (feed.given && frames < room) return true;
        }
        return true;
    }

    void reset() override { create(nullptr); }

    // AudioConverter answers each packet with that packet's audio (the
    // standard decoder alignment iTunes' 2112-frame priming is measured
    // against), so nothing is held back.
    bool drain(std::vector<float>&) override { return true; }
    int latency() const override { return 0; }

    int sampleRate() const override { return rate_; }
    int channels() const override { return channels_; }

private:
    AudioConverterRef conv_ = nullptr;
    AudioStreamBasicDescription in_{};
    AudioStreamBasicDescription out_{};
    std::vector<uint8_t> cookie_;
    int rate_ = 0;
    int channels_ = 0;
    UInt32 framesPerPacket_ = 1024;
};

} // namespace

std::unique_ptr<AacDecoder> makePlatformAacDecoder(const uint8_t* asc, size_t ascSize, std::string* error)
{
    auto d = std::make_unique<AtAacDecoder>();
    if (!d->init(asc, ascSize, error)) return nullptr;
    return d;
}

} // namespace broaudio
