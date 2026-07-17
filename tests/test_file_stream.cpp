// Disk-streamed playback: AudioFileStream incremental decoding, and the
// engine-level createStreamFromFile worker → SPSC ring → mixer path
// (prebuffer release, refill, underrun accounting, loop rewind, teardown).
//
// Pacing note: the decode worker is a real thread with a 50 ms wake cadence,
// while renderBlock() consumes at virtual speed. Tests real-sleep between
// render batches (or poll stats with a deadline) so the worker gets wall time
// to top the ring up — except the underrun test, which deliberately drains
// the ring faster than the worker can refill.

#include "test_harness.h"
#include "vorbis_fixtures.h"
#include "broaudio/engine.h"
#include "broaudio/io/audio_file.h"
#include "broaudio/io/audio_stream.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <thread>
#include <vector>

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;
static const char* WAV_PATH = "test_stream_tmp.wav";
static const char* OGG_PATH = "test_stream_tmp.ogg";

static std::vector<float> sineMono(int frames, float freq, int sr, float amp = 0.5f)
{
    std::vector<float> v(frames);
    for (int i = 0; i < frames; i++)
        v[i] = amp * std::sin(2.0f * PI * freq * i / sr);
    return v;
}

static bool writeBytes(const char* path, const unsigned char* data, size_t size)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, size, f);
    fclose(f);
    return w == size;
}

static float rmsRange(const std::vector<float>& v, size_t begin, size_t end)
{
    if (end > v.size()) end = v.size();
    if (begin >= end) return 0.0f;
    double acc = 0.0;
    for (size_t i = begin; i < end; i++) acc += static_cast<double>(v[i]) * v[i];
    return static_cast<float>(std::sqrt(acc / (end - begin)));
}

// Poll `pred` with real sleeps until it holds or ~5 s elapse.
template <typename F>
static bool waitFor(F pred, int timeoutMs = 5000)
{
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Render `frames` of audio in 512-frame blocks, pacing the drain to ~4x
// realtime so the 50 ms decode worker always refills the ring before the mixer
// can empty it. What matters for "no underrun" is the drain RATE, not the sleep
// granularity: consuming well under a ring's worth (the smallest here is 0.5 s)
// between two worker wakes holds on any OS timer — a coarser timer only sleeps
// longer, which is strictly safer. The old code slept a flat 8 ms per 4096
// frames (~11.6x realtime), which drained the 0.5 s ring faster than the worker
// woke and starved it wherever sleep_for was honored precisely (e.g. Linux).
// (The engine mixer itself never sleeps — this pacing lives only in the test.)
static void renderPaced(Engine& e, int frames)
{
    const int sr = e.sampleRate();
    int rendered = 0, sinceSleep = 0;
    while (rendered < frames) {
        int n = std::min(512, frames - rendered);
        e.renderBlock(n);
        rendered += n;
        sinceSleep += n;
        if (sinceSleep >= 4096) {
            // Sleep a quarter of the batch's real duration → ~4x realtime.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(sinceSleep * 1000 / (sr * 4)));
            sinceSleep = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// AudioFileStream (incremental decoder)
// ---------------------------------------------------------------------------

TEST(stream_wav_incremental_matches_full_decode) {
    const int sr = 44100, frames = sr * 2;
    auto sine = sineMono(frames, 440.0f, sr);
    ASSERT_TRUE(saveWav(WAV_PATH, sine.data(), frames, 1, sr));

    AudioFileStream s;
    ASSERT_TRUE(s.open(WAV_PATH));
    ASSERT_EQ(s.channels(), 1);
    ASSERT_EQ(s.sampleRate(), sr);
    ASSERT_EQ(static_cast<int>(s.totalFrames()), frames);

    // Pull in odd-sized chunks; the concatenation must equal the full decode.
    std::vector<float> streamed;
    std::vector<float> chunk(1000);
    int got;
    while ((got = s.readFrames(chunk.data(), 1000)) > 0)
        streamed.insert(streamed.end(), chunk.begin(), chunk.begin() + got);

    ASSERT_EQ(static_cast<int>(streamed.size()), frames);
    for (int i = 0; i < frames; i += 997)
        ASSERT_NEAR(streamed[i], sine[i], 1e-6f);

    // Rewind and confirm the first chunk repeats exactly.
    ASSERT_TRUE(s.seekToStart());
    got = s.readFrames(chunk.data(), 1000);
    ASSERT_EQ(got, 1000);
    for (int i = 0; i < 1000; i++)
        ASSERT_NEAR(chunk[i], sine[i], 1e-6f);

    s.close();
    std::remove(WAV_PATH);
    PASS();
}

TEST(stream_vorbis_incremental) {
    ASSERT_TRUE(writeBytes(OGG_PATH, fixture_mono_ogg, fixture_mono_ogg_len));

    AudioFileStream s;
    ASSERT_TRUE(s.open(OGG_PATH));
    ASSERT_EQ(s.channels(), 1);
    ASSERT_EQ(s.sampleRate(), 22050);

    std::vector<float> chunk(777);
    int total = 0, got;
    while ((got = s.readFrames(chunk.data(), 777)) > 0) total += got;
    // ~0.5 s at 22050 Hz
    ASSERT_TRUE(total > 22050 / 2 - 512 && total < 22050 / 2 + 512);

    ASSERT_TRUE(s.seekToStart());
    ASSERT_TRUE(s.readFrames(chunk.data(), 777) > 0);

    s.close();
    std::remove(OGG_PATH);
    PASS();
}

TEST(stream_open_errors) {
    AudioFileStream s;
    ASSERT_FALSE(s.open("no_such_file_xyz.wav"));
    ASSERT_FALSE(s.error().empty());

    // Garbage bytes: unrecognized format with a message that names the options
    unsigned char junk[256];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = static_cast<unsigned char>(i * 7);
    ASSERT_TRUE(writeBytes(WAV_PATH, junk, sizeof(junk)));
    ASSERT_FALSE(s.open(WAV_PATH));
    ASSERT_TRUE(s.error().find("WAV") != std::string::npos);
    std::remove(WAV_PATH);
    PASS();
}

// ---------------------------------------------------------------------------
// Engine::createStreamFromFile (worker → ring → mixer)
// ---------------------------------------------------------------------------

TEST(file_stream_plays_through_mixer_with_refills) {
    Engine e;
    ASSERT_TRUE(e.initHeadless());
    const int sr = e.sampleRate();

    // 3 s tone, ring only 0.5 s — playing it all requires multiple refills.
    auto sine = sineMono(sr * 3, 440.0f, sr);
    ASSERT_TRUE(saveWav(WAV_PATH, sine.data(), sr * 3, 1, sr));

    FileStreamOptions opts;
    opts.ringFrames = sr / 2;
    std::string err;
    int id = e.createStreamFromFile(WAV_PATH, opts, &err);
    ASSERT_TRUE(id >= 0);

    // Prebuffer decoded, playback released by the worker.
    ASSERT_TRUE(waitFor([&] { return e.getStreamStats(id).bufferedFrames >= static_cast<uint64_t>(sr / 4); }));

    e.startRecording();
    renderPaced(e, sr * 3 + sr / 2);  // render past the end of the file
    e.stopRecording();
    auto rec = e.getRecordBuffer();

    ASSERT_TRUE(waitFor([&] { return e.getStreamStats(id).finished; }));
    StreamStats st = e.getStreamStats(id);  // snapshot the final, settled state
    ASSERT_TRUE(st.valid);
    // The whole file was decoded through the 0.5 s ring (>= 6 refills).
    ASSERT_TRUE(st.decodedFrames >= static_cast<uint64_t>(sr * 3 - 4096));
    ASSERT_TRUE(st.playedFrames >= static_cast<uint64_t>(sr * 3 - 4096));
    ASSERT_EQ(static_cast<int>(st.underrunFrames), 0);

    // Audio actually flowed: strong signal early, silence after the end.
    ASSERT_TRUE(rmsRange(rec, sr / 2, sr) > 0.1f);
    ASSERT_TRUE(rmsRange(rec, rec.size() - sr / 4, rec.size()) < 0.01f);

    e.closeStream(id);
    ASSERT_FALSE(e.getStreamStats(id).valid);  // playback gone after close
    std::remove(WAV_PATH);
    PASS();
}

TEST(file_stream_underrun_counts_and_recovers) {
    Engine e;
    ASSERT_TRUE(e.initHeadless());
    const int sr = e.sampleRate();

    auto sine = sineMono(sr * 4, 330.0f, sr);
    ASSERT_TRUE(saveWav(WAV_PATH, sine.data(), sr * 4, 1, sr));

    // Tiny ring (~100 ms): draining 1 s of virtual audio in a few real ms
    // must starve the worker (50 ms wake cadence) → silence + counter.
    FileStreamOptions opts;
    opts.ringFrames = sr / 10;
    std::string err;
    int id = e.createStreamFromFile(WAV_PATH, opts, &err);
    ASSERT_TRUE(id >= 0);
    ASSERT_TRUE(waitFor([&] { return e.getStreamStats(id).bufferedFrames > 0; }));

    e.renderBlock(sr);  // 1 s of virtual audio, no real time for the worker
    StreamStats st = e.getStreamStats(id);
    ASSERT_TRUE(st.valid);
    ASSERT_TRUE(st.underrunFrames > 0);

    // The stream recovers once the worker gets wall time again.
    uint64_t playedBefore = st.playedFrames;
    ASSERT_TRUE(waitFor([&] { return e.getStreamStats(id).bufferedFrames >= static_cast<uint64_t>(sr / 20); }));
    e.startRecording();
    renderPaced(e, sr / 2);
    e.stopRecording();
    auto rec = e.getRecordBuffer();
    StreamStats st2 = e.getStreamStats(id);
    ASSERT_TRUE(st2.playedFrames > playedBefore);
    ASSERT_TRUE(rmsRange(rec, 0, rec.size()) > 0.05f);

    e.closeStream(id);
    std::remove(WAV_PATH);
    PASS();
}

TEST(file_stream_loops_seamlessly) {
    Engine e;
    ASSERT_TRUE(e.initHeadless());
    const int sr = e.sampleRate();

    // 0.3 s file, loop on: 1.5 s of playback must wrap the file ~5 times.
    const int fileFrames = sr * 3 / 10;
    auto sine = sineMono(fileFrames, 440.0f, sr);
    ASSERT_TRUE(saveWav(WAV_PATH, sine.data(), fileFrames, 1, sr));

    FileStreamOptions opts;
    opts.loop = true;
    std::string err;
    int id = e.createStreamFromFile(WAV_PATH, opts, &err);
    ASSERT_TRUE(id >= 0);
    ASSERT_TRUE(waitFor([&] { return e.getStreamStats(id).bufferedFrames > 0; }));

    e.startRecording();
    renderPaced(e, sr * 3 / 2);
    e.stopRecording();
    auto rec = e.getRecordBuffer();

    StreamStats st = e.getStreamStats(id);
    ASSERT_TRUE(st.playedFrames > static_cast<uint64_t>(fileFrames * 3));
    ASSERT_FALSE(st.finished);
    // No dead air anywhere: the last 0.2 s is still loud.
    ASSERT_TRUE(rmsRange(rec, rec.size() - sr / 5, rec.size()) > 0.1f);

    // Turn looping off: the stream then runs to its end.
    e.setPlaybackLoop(id, false);
    ASSERT_TRUE(waitFor([&] {
        e.renderBlock(2048);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return e.getStreamStats(id).finished;
    }));

    e.closeStream(id);
    std::remove(WAV_PATH);
    PASS();
}

TEST(file_stream_resamples_vorbis) {
    Engine e;
    ASSERT_TRUE(e.initHeadless());
    const int sr = e.sampleRate();
    ASSERT_TRUE(writeBytes(OGG_PATH, fixture_mono_ogg, fixture_mono_ogg_len));

    std::string err;
    int id = e.createStreamFromFile(OGG_PATH, &err);
    ASSERT_TRUE(id >= 0);
    ASSERT_TRUE(waitFor([&] { return e.getStreamStats(id).bufferedFrames > 0; }));

    e.startRecording();
    renderPaced(e, sr);  // file is 0.5 s — render 1 s
    e.stopRecording();
    auto rec = e.getRecordBuffer();

    // 0.5 s at 22050 resampled to the engine rate ≈ sr/2 frames decoded.
    StreamStats st = e.getStreamStats(id);
    ASSERT_TRUE(st.decodedFrames > static_cast<uint64_t>(sr / 2 - 4096));
    ASSERT_TRUE(st.decodedFrames < static_cast<uint64_t>(sr / 2 + 4096));
    ASSERT_TRUE(rmsRange(rec, sr / 10, sr / 4) > 0.1f);

    e.closeStream(id);
    std::remove(OGG_PATH);
    PASS();
}

TEST(file_stream_error_reporting) {
    Engine e;
    ASSERT_TRUE(e.initHeadless());

    std::string err;
    ASSERT_EQ(e.createStreamFromFile("no_such_file_xyz.mp3", &err), -1);
    ASSERT_FALSE(err.empty());

    unsigned char junk[128] = {0};
    ASSERT_TRUE(writeBytes(WAV_PATH, junk, sizeof(junk)));
    err.clear();
    ASSERT_EQ(e.createStreamFromFile(WAV_PATH, &err), -1);
    ASSERT_FALSE(err.empty());
    std::remove(WAV_PATH);
    PASS();
}

TEST(file_stream_teardown_paths) {
    const int fastSr = 44100;
    auto sine = sineMono(fastSr, 440.0f, fastSr);
    ASSERT_TRUE(saveWav(WAV_PATH, sine.data(), fastSr, 1, fastSr));

    // Close immediately after create (worker may still be prebuffering).
    {
        Engine e;
        ASSERT_TRUE(e.initHeadless());
        std::string err;
        int id = e.createStreamFromFile(WAV_PATH, &err);
        ASSERT_TRUE(id >= 0);
        e.closeStream(id);
        ASSERT_FALSE(e.getStreamStats(id).valid);
    }

    // Engine destroyed with a live stream — shutdown joins the worker.
    {
        Engine e;
        ASSERT_TRUE(e.initHeadless());
        std::string err;
        int id = e.createStreamFromFile(WAV_PATH, &err);
        ASSERT_TRUE(id >= 0);
        e.renderBlock(4096);
    }

    // Double close is harmless.
    {
        Engine e;
        ASSERT_TRUE(e.initHeadless());
        std::string err;
        int id = e.createStreamFromFile(WAV_PATH, &err);
        ASSERT_TRUE(id >= 0);
        e.closeStream(id);
        e.closeStream(id);
    }

    std::remove(WAV_PATH);
    PASS();
}

int main() { return runAllTests(); }
