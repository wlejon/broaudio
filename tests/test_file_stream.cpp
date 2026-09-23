// Disk-streamed playback: AudioFileStream incremental decoding, and the
// engine-level createStreamFromFile worker → SPSC ring → mixer path
// (prebuffer release, refill, underrun accounting, loop rewind, teardown).
//
// Pacing note: the decode worker is a real thread with a 50 ms wake cadence,
// while renderBlock() consumes at virtual speed. Tests that must not starve
// the ring feed the mixer only what the worker has already buffered (see
// renderFed), so they hold however slowly the worker is scheduled — a
// wall-clock pace starved it under `ctest -j8`. The underrun test drains the
// ring faster than the worker can refill on purpose.

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

// Poll `pred` with real sleeps until it holds or the deadline passes. The
// deadline only bounds a failing run, so it is generous: a loaded machine
// may take seconds to schedule the worker.
template <typename F>
static bool waitFor(F pred, int timeoutMs = 20000)
{
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Render `frames` of audio for stream `id` without ever starving it: each
// block is at most what the ring already holds, and once the ring is empty
// the next block waits until the worker refills it or the stream has ended
// (after which rendering on is silence the mixer does not count). A
// wall-clock pace (the old ~4x realtime) held only while the OS scheduled the
// worker within a ring's worth of drain, which a loaded machine does not
// promise. Returns false if the worker made no progress before the deadline.
static bool renderFed(Engine& e, int id, int frames)
{
    if (!waitFor([&] { return e.isPlaybackPlaying(id); })) return false;
    int rendered = 0;
    while (rendered < frames) {
        const int want = std::min(512, frames - rendered);
        int n = 0;
        if (!waitFor([&] {
                StreamStats st = e.getStreamStats(id);
                if (st.bufferedFrames > 0) {
                    n = static_cast<int>(std::min<uint64_t>(st.bufferedFrames, want));
                    return true;
                }
                if (st.finished) { n = want; return true; }
                return false;
            }))
            return false;
        e.renderBlock(n);
        rendered += n;
    }
    return true;
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
    ASSERT_TRUE(renderFed(e, id, sr * 3 + sr / 2));  // render past the end of the file
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
    // Playing, not just buffered: until the worker releases playback the
    // mixer reads nothing, so there would be nothing to starve.
    ASSERT_TRUE(waitFor([&] { return e.isPlaybackPlaying(id); }));

    e.renderBlock(sr);  // 1 s of virtual audio, no real time for the worker
    StreamStats st = e.getStreamStats(id);
    ASSERT_TRUE(st.valid);
    ASSERT_TRUE(st.underrunFrames > 0);

    // The stream recovers once the worker gets wall time again.
    uint64_t playedBefore = st.playedFrames;
    ASSERT_TRUE(waitFor([&] { return e.getStreamStats(id).bufferedFrames >= static_cast<uint64_t>(sr / 20); }));
    e.startRecording();
    ASSERT_TRUE(renderFed(e, id, sr / 2));
    e.stopRecording();
    auto rec = e.getRecordBuffer();
    StreamStats st2 = e.getStreamStats(id);
    ASSERT_TRUE(st2.playedFrames > playedBefore);
    ASSERT_TRUE(rmsRange(rec, 0, rec.size()) > 0.05f);

    e.closeStream(id);
    std::remove(WAV_PATH);
    PASS();
}

// A ring shorter than the worker's decode chunk must still be topped up after
// a *partial* drain. The worker used to require a whole chunk of free space
// before decoding, so a ring smaller than two chunks wedged at whatever level
// the last drain left it — the mixer only consumes while the host renders, so
// nothing ever freed the space the worker was waiting for.
TEST(file_stream_small_ring_tops_up_after_partial_drain) {
    Engine e;
    ASSERT_TRUE(e.initHeadless());
    const int sr = e.sampleRate();

    auto sine = sineMono(sr * 4, 330.0f, sr);
    ASSERT_TRUE(saveWav(WAV_PATH, sine.data(), sr * 4, 1, sr));

    FileStreamOptions opts;
    opts.ringFrames = sr / 10;  // ~100 ms — well under one 4096-frame chunk
    std::string err;
    int id = e.createStreamFromFile(WAV_PATH, opts, &err);
    ASSERT_TRUE(id >= 0);
    ASSERT_TRUE(waitFor([&] { return e.isPlaybackPlaying(id); }));

    // Drain by assorted partial amounts, from both sides of the old dead zone.
    // Whatever the level lands on, the worker must keep the ring at least half
    // full rather than parking below it.
    for (int drain : {2048, 1000, 500, 2200, 4000}) {
        e.renderBlock(drain);
        ASSERT_TRUE(waitFor([&] {
            return e.getStreamStats(id).bufferedFrames
                   >= static_cast<uint64_t>(opts.ringFrames / 2);
        }));
    }

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
    e.startRecording();
    ASSERT_TRUE(renderFed(e, id, sr * 3 / 2));
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
    e.startRecording();
    ASSERT_TRUE(renderFed(e, id, sr));  // file is 0.5 s — render 1 s
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

// ---------------------------------------------------------------------------
// Seek (Engine::seekPlayback → decoder seek + ring flush fence)
// ---------------------------------------------------------------------------

TEST(file_stream_seek_lands_in_file_time) {
    const int sr = 44100;
    // First second silence, second second a loud tone: audio right after a
    // seek to 1.5 s proves the decoder really jumped (from 0 we'd get
    // silence for a full second).
    std::vector<float> pcm(sr * 2, 0.0f);
    auto tone = sineMono(sr, 440.0f, sr);
    std::copy(tone.begin(), tone.end(), pcm.begin() + sr);
    ASSERT_TRUE(saveWav(WAV_PATH, pcm.data(), sr * 2, 1, sr));

    Engine e;
    ASSERT_TRUE(e.initHeadless());
    std::string err;
    int id = e.createStreamFromFile(WAV_PATH, &err);
    ASSERT_TRUE(id >= 0);
    ASSERT_TRUE(waitFor([&] { return e.getStreamStats(id).valid
                                  && e.getStreamStats(id).bufferedFrames > 0; }));

    e.seekPlayback(id, 1.5);

    // Render in small blocks until post-seek tone audio arrives (worker may
    // take a few wakes to seek + refill; pre-seek buffered silence is fenced
    // off, so the first non-silent block IS post-seek audio).
    bool heard = waitFor([&] {
        e.renderBlock(512);
        return e.getBusPeakL(Engine::MASTER_BUS_ID) > 0.05f
            || e.getBusPeakR(Engine::MASTER_BUS_ID) > 0.05f;
    });
    ASSERT_TRUE(heard);

    // Position reports file time at/after the seek target.
    double pos = e.getPlaybackPositionSeconds(id);
    ASSERT_TRUE(pos >= 1.45);
    ASSERT_TRUE(pos <= 2.1);

    e.closeStream(id);
    std::remove(WAV_PATH);
    PASS();
}

TEST(file_stream_seek_restarts_finished_stream) {
    const int sr = 44100;
    auto tone = sineMono(sr / 2, 440.0f, sr);   // 0.5 s tone
    ASSERT_TRUE(saveWav(WAV_PATH, tone.data(), sr / 2, 1, sr));

    Engine e;
    ASSERT_TRUE(e.initHeadless());
    std::string err;
    int id = e.createStreamFromFile(WAV_PATH, &err);
    ASSERT_TRUE(id >= 0);

    // Drain to EOF.
    ASSERT_TRUE(waitFor([&] {
        e.renderBlock(4096);
        return e.getStreamStats(id).finished;
    }));

    // Seek back — the worker re-opens decode from 0.1 s and audio flows again.
    e.seekPlayback(id, 0.1);
    ASSERT_TRUE(waitFor([&] {
        e.renderBlock(512);
        return e.getBusPeakL(Engine::MASTER_BUS_ID) > 0.05f;
    }));
    ASSERT_FALSE(e.getStreamStats(id).finished);

    e.closeStream(id);
    std::remove(WAV_PATH);
    PASS();
}

int main() { return runAllTests(); }
