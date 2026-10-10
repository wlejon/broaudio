// MP4/M4A AAC decoding through the platform decoder (Media Foundation on
// Windows, AudioToolbox on macOS): loadAudioFile, loadAudioFileFromMemory and
// AudioFileStream over tests/fixtures/tone_stereo.m4a — ffmpeg's AAC-LC,
// 0.5 s, 44.1 kHz stereo, L = 440 Hz, R = 880 Hz at 0.5, with an edit list
// that trims the encoder's 1024 priming frames. Where the build has no AAC
// decoder (Linux), every entry point must fail with the error that says so.

#include "test_harness.h"
#include "broaudio/io/audio_file.h"
#include "broaudio/io/audio_stream.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace broaudio;

static const std::string kPath = std::string(BROAUDIO_TEST_FIXTURES) + "/tone_stereo.m4a";

static std::vector<uint8_t> fileBytes(const std::string& path)
{
    std::vector<uint8_t> out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
        fclose(f);
    }
    return out;
}

static double rms(const std::vector<float>& v, int ch, int offset, size_t from, size_t to)
{
    double s = 0;
    size_t n = 0;
    for (size_t i = from; i < to && i * ch + offset < v.size(); ++i, ++n) {
        const double x = v[i * ch + offset];
        s += x * x;
    }
    return n ? std::sqrt(s / n) : 0.0;
}

static double zeroCrossHz(const std::vector<float>& v, int ch, int offset, size_t from, size_t to, int rate)
{
    int c = 0;
    float prev = v[from * ch + offset];
    for (size_t i = from + 1; i < to; ++i) {
        const float x = v[i * ch + offset];
        if ((prev < 0 && x >= 0) || (prev >= 0 && x < 0)) ++c;
        prev = x;
    }
    return c / (2.0 * double(to - from) / rate);
}

#if defined(BROAUDIO_HAS_AAC) && BROAUDIO_HAS_AAC

static bool checkTone(const AudioFileData& d)
{
    if (!d.valid()) { std::printf("  decode failed: %s\n", d.error.c_str()); return false; }
    if (d.channels != 2 || d.sampleRate != 44100) return false;
    // The edit list trims the priming: exactly the 0.5 s that was encoded.
    if (d.numFrames != 22050) { std::printf("  frames %d\n", d.numFrames); return false; }
    const double l = rms(d.samples, 2, 0, 2000, 20000), r = rms(d.samples, 2, 1, 2000, 20000);
    const double hl = zeroCrossHz(d.samples, 2, 0, 2000, 20000, 44100);
    const double hr = zeroCrossHz(d.samples, 2, 1, 2000, 20000, 44100);
    std::printf("  rms L %.3f R %.3f, pitch L %.1f Hz R %.1f Hz\n", l, r, hl, hr);
    // The tone starts at the first frame: no priming silence left in front.
    const double head = rms(d.samples, 2, 0, 0, 256);
    size_t onset = 0;
    while (onset < size_t(d.numFrames) && std::fabs(d.samples[onset * 2]) < 0.05) ++onset;
    // And it runs to the last frame (a decoder with latency was drained).
    // The encoder tapers the end: ffmpeg's own decoder reads 0.188 there too
    // (and 0.349 for the first 256), so both ends match it.
    const double tail = rms(d.samples, 2, 0, 22050 - 256, 22050);
    std::printf("  rms of the first 256 frames %.3f (onset at frame %zu), of the last 256 %.3f\n", head, onset, tail);
    return std::fabs(l - 0.3536) < 0.03 && std::fabs(r - 0.3536) < 0.03 &&
           std::fabs(hl - 440) < 5 && std::fabs(hr - 880) < 8 && std::fabs(head - 0.349) < 0.02 && onset < 8 && std::fabs(tail - 0.188) < 0.02;
}

TEST(load_file)
{
    ASSERT_TRUE(checkTone(loadAudioFile(kPath.c_str())));
    g_testsPassed++;
}

TEST(load_memory)
{
    const std::vector<uint8_t> bytes = fileBytes(kPath);
    ASSERT_TRUE(!bytes.empty());
    ASSERT_TRUE(checkTone(loadAudioFileFromMemory(bytes.data(), bytes.size())));
    g_testsPassed++;
}

TEST(stream_and_seek)
{
    const AudioFileData whole = loadAudioFile(kPath.c_str());
    ASSERT_TRUE(whole.valid());

    AudioFileStream s;
    ASSERT_TRUE(s.open(kPath.c_str()));
    ASSERT_EQ(s.channels(), 2);
    ASSERT_EQ(s.sampleRate(), 44100);
    ASSERT_EQ(s.totalFrames(), uint64_t(22050));

    // Streamed in odd-sized chunks, the same samples as the whole decode.
    std::vector<float> got;
    std::vector<float> buf(777 * 2);
    for (;;) {
        const int n = s.readFrames(buf.data(), 777);
        if (n <= 0) break;
        got.insert(got.end(), buf.begin(), buf.begin() + n * 2);
    }
    ASSERT_EQ(got.size(), whole.samples.size());
    double maxDiff = 0;
    for (size_t i = 0; i < got.size(); ++i) maxDiff = std::fmax(maxDiff, std::fabs(got[i] - whole.samples[i]));
    ASSERT_TRUE(maxDiff < 1e-6);

    // A seek lands on the frame asked for (decoded from two packets before it).
    for (uint64_t at : {uint64_t(0), uint64_t(1), uint64_t(5000), uint64_t(11025), uint64_t(22049)}) {
        ASSERT_TRUE(s.seekToFrame(at));
        const int n = s.readFrames(buf.data(), 512);
        ASSERT_EQ(n, static_cast<int>(std::min<uint64_t>(512, 22050 - at)));
        double d = 0;
        for (int i = 0; i < n * 2; ++i) d = std::fmax(d, std::fabs(buf[i] - whole.samples[at * 2 + i]));
        if (d > 2e-3) std::printf("  seek to %llu: max diff %f\n", static_cast<unsigned long long>(at), d);
        ASSERT_TRUE(d < 2e-3);
    }
    ASSERT_FALSE(s.seekToFrame(22051));
    ASSERT_TRUE(s.seekToStart());
    ASSERT_EQ(s.readFrames(buf.data(), 10), 10);
    g_testsPassed++;
}

TEST(not_aac)
{
    // An MP4 that is not one: a clear error, not a crash.
    const std::vector<uint8_t> bytes = {0, 0, 0, 16, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ', 0, 0, 0, 0};
    AudioFileData d = loadAudioFileFromMemory(bytes.data(), bytes.size());
    ASSERT_FALSE(d.valid());
    ASSERT_FALSE(d.error.empty());
    g_testsPassed++;
}

#else

TEST(no_platform_decoder)
{
    AudioFileData d = loadAudioFile(kPath.c_str());
    ASSERT_FALSE(d.valid());
    ASSERT_TRUE(d.error.find("platform AAC decoder") != std::string::npos);
    AudioFileStream s;
    ASSERT_FALSE(s.open(kPath.c_str()));
    ASSERT_TRUE(s.error().find("platform AAC decoder") != std::string::npos);
    g_testsPassed++;
}

#endif

int main() { return runAllTests(); }
