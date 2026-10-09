// The device layer (broaudio/device.h) through the null backend, which runs
// the realtime path on a timer thread with no sound hardware: Engine::init
// with an explicit backend, the output callback advancing the clock and
// producing audio, mic capture through the backend, stream info and stats,
// backend selection by name, and shutdown stopping the callbacks.

#include "test_harness.h"
#include "broaudio/engine.h"
#include "broaudio/device.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace broaudio;

static void waitMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

TEST(backend_names_round_trip) {
    AudioBackendKind k = AudioBackendKind::Auto;
    ASSERT_TRUE(parseAudioBackendKind("PipeWire", &k));
    ASSERT_TRUE(k == AudioBackendKind::PipeWire);
    ASSERT_TRUE(parseAudioBackendKind("sdl", &k));
    ASSERT_TRUE(k == AudioBackendKind::Sdl);
    ASSERT_TRUE(parseAudioBackendKind("dummy", &k));
    ASSERT_TRUE(k == AudioBackendKind::Null);
    ASSERT_FALSE(parseAudioBackendKind("alsa", &k));
    ASSERT_TRUE(k == AudioBackendKind::Null);
    ASSERT_TRUE(std::strcmp(audioBackendKindName(AudioBackendKind::PipeWire), "pipewire") == 0);
    ASSERT_TRUE(audioBackendCompiled(AudioBackendKind::Sdl));
    ASSERT_TRUE(audioBackendCompiled(AudioBackendKind::Null));
    PASS();
}

TEST(null_backend_stream_runs_and_stops) {
    auto backend = createAudioBackend(AudioBackendKind::Null);
    ASSERT_TRUE(backend != nullptr);
    ASSERT_TRUE(backend->kind() == AudioBackendKind::Null);
    ASSERT_EQ(static_cast<int>(backend->devices(AudioDirection::Playback).size()), 1);

    std::atomic<int> calls{0};
    std::atomic<int> frames{0};
    AudioStreamConfig cfg;
    cfg.sampleRate = 48000;
    cfg.channels = 2;
    cfg.periodFrames = 256;
    struct Ctx { std::atomic<int>* calls; std::atomic<int>* frames; } ctx{&calls, &frames};
    auto stream = backend->openStream(cfg, [](void* u, float* buf, int n) {
        auto* c = static_cast<Ctx*>(u);
        for (int i = 0; i < n * 2; ++i) buf[i] = 0.0f;
        c->calls->fetch_add(1);
        c->frames->fetch_add(n);
    }, &ctx);
    ASSERT_TRUE(stream != nullptr);
    waitMs(30);
    ASSERT_EQ(calls.load(), 0);   // opened stopped

    ASSERT_TRUE(stream->start());
    waitMs(120);
    stream->stop();
    const int after = calls.load();
    ASSERT_GT(after, 5);
    ASSERT_EQ(frames.load(), after * 256);
    waitMs(30);
    ASSERT_EQ(calls.load(), after);   // nothing after stop()

    AudioStreamInfo info = stream->info();
    ASSERT_EQ(info.periodFrames, 256);
    ASSERT_NEAR(info.latencySeconds, 256.0 / 48000.0, 1e-9);
    ASSERT_EQ(static_cast<int>(stream->stats().callbacks), after);
    PASS();
}

TEST(engine_init_on_null_backend) {
    Engine e;
    AudioDeviceConfig cfg;
    cfg.backend = AudioBackendKind::Null;
    cfg.appId = "org.broaudio.test";
    ASSERT_TRUE(e.init(cfg));
    ASSERT_TRUE(std::strcmp(e.audioBackendName(), "null") == 0);
    ASSERT_GT(e.outputLatencySeconds(), 0.0);

    int v = e.createVoice();
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.startVoice(v, 0.0);

    waitMs(150);
    ASSERT_GT(e.currentTime(), 0.03);   // the device callback drives the clock
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.001f);
    ASSERT_GT(static_cast<int>(e.outputStreamStats().callbacks), 3);
    ASSERT_EQ(static_cast<int>(e.audioDevices(AudioDirection::Capture).size()), 1);

    // The mic opens through the same backend; null capture is silence.
    std::atomic<int> tapSamples{0};
    MicTapId tap = e.addMicTap({}, [&](const float*, int n) { tapSamples.fetch_add(n); });
    ASSERT_TRUE(e.startMicCapture());
    waitMs(100);
    ASSERT_GT(tapSamples.load(), 1000);
    ASSERT_GT(e.inputLatencySeconds(), 0.0);
    e.stopMicCapture();
    e.removeMicTap(tap);
    ASSERT_TRUE(e.inputLatencySeconds() == 0.0);

    e.update();   // pollEvents on a backend with none: harmless
    e.shutdown();
    const double t = e.currentTime();
    waitMs(50);
    ASSERT_TRUE(e.currentTime() == t);   // no callbacks after shutdown
    ASSERT_TRUE(std::strcmp(e.audioBackendName(), "none") == 0);
    PASS();
}

TEST(headless_engine_has_no_backend) {
    Engine e;
    ASSERT_TRUE(e.initHeadless());
    ASSERT_TRUE(std::strcmp(e.audioBackendName(), "none") == 0);
    ASSERT_TRUE(e.outputLatencySeconds() == 0.0);
    ASSERT_TRUE(e.audioDevices(AudioDirection::Playback).empty());
    ASSERT_FALSE(e.startMicCapture());   // headless never claims the mic
    ASSERT_FALSE(e.isMicCapturing());
    PASS();
}

int main() { return runAllTests(); }
