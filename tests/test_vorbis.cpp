// Ogg Vorbis decoding (stb_vorbis): mono + stereo fixtures, file and memory
// paths, resample-on-load through Engine::createClipFromFile, and actionable
// errors for corrupt input. Fixtures are embedded (vorbis_fixtures.h): 0.5 s
// 440 Hz sine tones encoded with ffmpeg/libvorbis at low bitrate.

#include "test_harness.h"
#include "vorbis_fixtures.h"
#include "broaudio/engine.h"
#include "broaudio/io/audio_file.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace broaudio;

static const char* TEST_OGG_PATH = "test_vorbis_tmp.ogg";

static bool writeBytes(const char* path, const unsigned char* data, size_t size)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, size, f);
    fclose(f);
    return w == size;
}

static float rmsOfChannel(const AudioFileData& d, int ch)
{
    double acc = 0.0;
    for (int i = 0; i < d.numFrames; i++) {
        float s = d.samples[static_cast<size_t>(i) * d.channels + ch];
        acc += static_cast<double>(s) * s;
    }
    return d.numFrames > 0 ? static_cast<float>(std::sqrt(acc / d.numFrames)) : 0.0f;
}

// Estimate the dominant frequency of one channel by zero-crossing count.
static float zeroCrossingHz(const AudioFileData& d, int ch)
{
    int crossings = 0;
    float prev = d.samples[ch];
    for (int i = 1; i < d.numFrames; i++) {
        float s = d.samples[static_cast<size_t>(i) * d.channels + ch];
        if ((prev < 0.0f && s >= 0.0f) || (prev >= 0.0f && s < 0.0f)) crossings++;
        prev = s;
    }
    float seconds = static_cast<float>(d.numFrames) / d.sampleRate;
    return seconds > 0.0f ? crossings / (2.0f * seconds) : 0.0f;
}

TEST(vorbis_mono_from_memory) {
    AudioFileData d = loadAudioFileFromMemory(fixture_mono_ogg, fixture_mono_ogg_len);
    ASSERT_TRUE(d.valid());
    ASSERT_TRUE(d.error.empty());
    ASSERT_EQ(d.channels, 1);
    ASSERT_EQ(d.sampleRate, 22050);
    // 0.5 s at 22050 Hz — Vorbis blocks may trim edges slightly
    ASSERT_TRUE(d.numFrames > 22050 / 2 - 512 && d.numFrames < 22050 / 2 + 512);
    ASSERT_EQ(static_cast<int>(d.samples.size()), d.numFrames * d.channels);

    // A 0.5-amplitude sine: RMS near 0.35, all samples finite and sane
    float rms = rmsOfChannel(d, 0);
    ASSERT_TRUE(rms > 0.25f && rms < 0.45f);
    for (float s : d.samples) ASSERT_TRUE(std::isfinite(s) && std::fabs(s) < 1.5f);

    // Tone frequency survives the codec: ~440 Hz
    float hz = zeroCrossingHz(d, 0);
    ASSERT_NEAR(hz, 440.0f, 25.0f);
    PASS();
}

TEST(vorbis_stereo_from_memory) {
    AudioFileData d = loadAudioFileFromMemory(fixture_stereo_ogg, fixture_stereo_ogg_len);
    ASSERT_TRUE(d.valid());
    ASSERT_EQ(d.channels, 2);
    ASSERT_EQ(d.sampleRate, 44100);
    ASSERT_TRUE(d.numFrames > 22050 - 1024 && d.numFrames < 22050 + 1024);

    // Interleaving is real: L is 440 Hz, R is 880 Hz
    float hzL = zeroCrossingHz(d, 0);
    float hzR = zeroCrossingHz(d, 1);
    ASSERT_NEAR(hzL, 440.0f, 30.0f);
    ASSERT_NEAR(hzR, 880.0f, 50.0f);
    ASSERT_TRUE(rmsOfChannel(d, 0) > 0.25f);
    ASSERT_TRUE(rmsOfChannel(d, 1) > 0.25f);
    PASS();
}

TEST(vorbis_from_file_path) {
    ASSERT_TRUE(writeBytes(TEST_OGG_PATH, fixture_mono_ogg, fixture_mono_ogg_len));
    AudioFileData d = loadAudioFile(TEST_OGG_PATH);
    std::remove(TEST_OGG_PATH);

    ASSERT_TRUE(d.valid());
    ASSERT_EQ(d.channels, 1);
    ASSERT_EQ(d.sampleRate, 22050);
    ASSERT_NEAR(zeroCrossingHz(d, 0), 440.0f, 25.0f);
    PASS();
}

TEST(vorbis_truncated_reports_error) {
    // Cut mid-stream: headers parse or not depending on where the cut lands —
    // either way the result must be invalid with a non-empty, actionable error.
    AudioFileData d = loadAudioFileFromMemory(fixture_mono_ogg, 120);
    ASSERT_FALSE(d.valid());
    ASSERT_FALSE(d.error.empty());
    ASSERT_TRUE(d.error.find("Vorbis") != std::string::npos);
    PASS();
}

TEST(vorbis_corrupt_header_reports_error) {
    // Valid Ogg capture pattern followed by garbage — sniffs as Vorbis (we
    // preserve the id packet), then the setup must fail cleanly.
    std::vector<unsigned char> bad(fixture_mono_ogg, fixture_mono_ogg + 512);
    for (size_t i = 64; i < bad.size(); i++) bad[i] ^= 0xA5;
    AudioFileData d = loadAudioFileFromMemory(bad.data(), bad.size());
    ASSERT_FALSE(d.valid());
    ASSERT_FALSE(d.error.empty());
    PASS();
}

TEST(vorbis_truncated_file_path_reports_error) {
    ASSERT_TRUE(writeBytes(TEST_OGG_PATH, fixture_mono_ogg, 120));
    AudioFileData d = loadAudioFile(TEST_OGG_PATH);
    std::remove(TEST_OGG_PATH);
    ASSERT_FALSE(d.valid());
    ASSERT_FALSE(d.error.empty());
    PASS();
}

TEST(vorbis_clip_from_file_resamples_to_engine_rate) {
    Engine e;
    ASSERT_TRUE(e.initHeadless());

    ASSERT_TRUE(writeBytes(TEST_OGG_PATH, fixture_mono_ogg, fixture_mono_ogg_len));
    int clipId = e.createClipFromFile(TEST_OGG_PATH);
    std::remove(TEST_OGG_PATH);

    ASSERT_TRUE(clipId >= 0);
    ASSERT_EQ(e.getClipChannels(clipId), 1);
    // 0.5 s of audio resampled from 22050 to the engine rate
    int expect = e.sampleRate() / 2;
    int frames = e.getClipSampleCount(clipId);
    ASSERT_TRUE(frames > expect - 2048 && frames < expect + 2048);
    e.deleteClip(clipId);
    PASS();
}

int main() { return runAllTests(); }
