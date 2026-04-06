#include "test_harness.h"
#include "broaudio/io/audio_file.h"
#include "broaudio/engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace broaudio;

// Generate a 440 Hz sine at the given sample rate, mono, 1 second.
static std::vector<float> generateSine(int sampleRate, float freq = 440.0f) {
    int frames = sampleRate;
    std::vector<float> buf(frames);
    for (int i = 0; i < frames; i++)
        buf[i] = std::sin(2.0f * std::numbers::pi_v<float> * freq * i / sampleRate);
    return buf;
}

static const char* TEST_WAV_PATH = "test_roundtrip.wav";

TEST(wav_save_and_load_mono) {
    int sr = 44100;
    auto sine = generateSine(sr);

    bool saved = saveWav(TEST_WAV_PATH, sine.data(), static_cast<int>(sine.size()), 1, sr);
    ASSERT_TRUE(saved);

    AudioFileData loaded = loadAudioFile(TEST_WAV_PATH);
    ASSERT_TRUE(loaded.valid());
    ASSERT_EQ(loaded.channels, 1);
    ASSERT_EQ(loaded.sampleRate, sr);
    ASSERT_EQ(loaded.numFrames, static_cast<int>(sine.size()));

    // Check samples match (float32 WAV should be exact)
    for (int i = 0; i < 100; i++) {
        ASSERT_NEAR(loaded.samples[i], sine[i], 1e-6f);
    }

    std::remove(TEST_WAV_PATH);
    PASS();
}

TEST(wav_save_and_load_stereo) {
    int sr = 44100;
    int frames = sr / 2;  // 0.5 seconds
    std::vector<float> stereo(frames * 2);
    for (int i = 0; i < frames; i++) {
        float s = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / sr);
        stereo[i * 2] = s;
        stereo[i * 2 + 1] = -s;  // inverted right channel
    }

    bool saved = saveWav(TEST_WAV_PATH, stereo.data(), frames, 2, sr);
    ASSERT_TRUE(saved);

    AudioFileData loaded = loadAudioFile(TEST_WAV_PATH);
    ASSERT_TRUE(loaded.valid());
    ASSERT_EQ(loaded.channels, 2);
    ASSERT_EQ(loaded.sampleRate, sr);
    ASSERT_EQ(loaded.numFrames, frames);
    ASSERT_EQ(static_cast<int>(loaded.samples.size()), frames * 2);

    // Verify interleaving preserved
    for (int i = 0; i < 50; i++) {
        ASSERT_NEAR(loaded.samples[i * 2], stereo[i * 2], 1e-6f);
        ASSERT_NEAR(loaded.samples[i * 2 + 1], stereo[i * 2 + 1], 1e-6f);
    }

    std::remove(TEST_WAV_PATH);
    PASS();
}

TEST(wav_load_from_memory) {
    int sr = 44100;
    auto sine = generateSine(sr, 220.0f);

    // Save to file, then read raw bytes, then load from memory
    saveWav(TEST_WAV_PATH, sine.data(), static_cast<int>(sine.size()), 1, sr);

    FILE* f = std::fopen(TEST_WAV_PATH, "rb");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> rawBytes(fileSize);
    std::fread(rawBytes.data(), 1, fileSize, f);
    std::fclose(f);

    AudioFileData loaded = loadAudioFileFromMemory(rawBytes.data(), rawBytes.size());
    ASSERT_TRUE(loaded.valid());
    ASSERT_EQ(loaded.channels, 1);
    ASSERT_EQ(loaded.sampleRate, sr);
    ASSERT_EQ(loaded.numFrames, static_cast<int>(sine.size()));

    for (int i = 0; i < 100; i++) {
        ASSERT_NEAR(loaded.samples[i], sine[i], 1e-6f);
    }

    std::remove(TEST_WAV_PATH);
    PASS();
}

TEST(load_nonexistent_file_returns_invalid) {
    AudioFileData data = loadAudioFile("nonexistent_file_12345.wav");
    ASSERT_FALSE(data.valid());
    PASS();
}

TEST(load_null_path_returns_invalid) {
    AudioFileData data = loadAudioFile(nullptr);
    ASSERT_FALSE(data.valid());
    PASS();
}

TEST(load_from_empty_memory_returns_invalid) {
    AudioFileData data = loadAudioFileFromMemory(nullptr, 0);
    ASSERT_FALSE(data.valid());

    uint8_t tiny[] = {0, 0};
    data = loadAudioFileFromMemory(tiny, 2);
    ASSERT_FALSE(data.valid());

    PASS();
}

TEST(save_wav_rejects_bad_params) {
    float dummy = 1.0f;
    ASSERT_FALSE(saveWav(nullptr, &dummy, 1, 1, 44100));
    ASSERT_FALSE(saveWav(TEST_WAV_PATH, nullptr, 1, 1, 44100));
    ASSERT_FALSE(saveWav(TEST_WAV_PATH, &dummy, 0, 1, 44100));
    ASSERT_FALSE(saveWav(TEST_WAV_PATH, &dummy, 1, 0, 44100));
    ASSERT_FALSE(saveWav(TEST_WAV_PATH, &dummy, 1, 1, 0));
    PASS();
}

// ---------------------------------------------------------------------------
// Async loading & size limit (require Engine in headless mode)
// ---------------------------------------------------------------------------

TEST(create_clip_from_file_async) {
    // Save a test WAV
    int sr = 44100;
    auto sine = generateSine(sr);
    saveWav(TEST_WAV_PATH, sine.data(), static_cast<int>(sine.size()), 1, sr);

    Engine engine;
    ASSERT_TRUE(engine.initHeadless());

    auto future = engine.createClipFromFileAsync(TEST_WAV_PATH);
    int clipId = future.get();
    ASSERT_TRUE(clipId >= 0);
    ASSERT_EQ(engine.getClipChannels(clipId), 1);
    ASSERT_EQ(engine.getClipSampleCount(clipId), static_cast<int>(sine.size()));

    engine.shutdown();
    std::remove(TEST_WAV_PATH);
    PASS();
}

TEST(async_load_nonexistent_returns_negative) {
    Engine engine;
    ASSERT_TRUE(engine.initHeadless());

    auto future = engine.createClipFromFileAsync("does_not_exist.wav");
    int clipId = future.get();
    ASSERT_EQ(clipId, -1);

    engine.shutdown();
    PASS();
}

TEST(async_load_null_path_returns_negative) {
    Engine engine;
    ASSERT_TRUE(engine.initHeadless());

    auto future = engine.createClipFromFileAsync(nullptr);
    int clipId = future.get();
    ASSERT_EQ(clipId, -1);

    engine.shutdown();
    PASS();
}

TEST(size_limit_rejects_large_clip) {
    int sr = 44100;
    auto sine = generateSine(sr);
    saveWav(TEST_WAV_PATH, sine.data(), static_cast<int>(sine.size()), 1, sr);

    Engine engine;
    ASSERT_TRUE(engine.initHeadless());

    // Set a tiny limit (1 KB) — the 1-second 44100-sample clip will exceed it
    engine.setMaxClipDecodedBytes(1024);
    int clipId = engine.createClipFromFile(TEST_WAV_PATH);
    ASSERT_EQ(clipId, -1);

    // Disable limit — should succeed
    engine.setMaxClipDecodedBytes(0);
    clipId = engine.createClipFromFile(TEST_WAV_PATH);
    ASSERT_TRUE(clipId >= 0);

    engine.shutdown();
    std::remove(TEST_WAV_PATH);
    PASS();
}

TEST(size_limit_rejects_large_clip_async) {
    int sr = 44100;
    auto sine = generateSine(sr);
    saveWav(TEST_WAV_PATH, sine.data(), static_cast<int>(sine.size()), 1, sr);

    Engine engine;
    ASSERT_TRUE(engine.initHeadless());

    engine.setMaxClipDecodedBytes(1024);
    auto future = engine.createClipFromFileAsync(TEST_WAV_PATH);
    ASSERT_EQ(future.get(), -1);

    engine.shutdown();
    std::remove(TEST_WAV_PATH);
    PASS();
}

int main() { return runAllTests(); }
