// audio_device_probe: opens the engine on a real device and reports what the
// device layer measures, once a second. Not a ctest (it needs sound
// hardware); it is the tool behind the device-layer measurements.
//
//   audio_device_probe [--seconds N] [--backend pipewire|sdl|null]
//                      [--period FRAMES] [--app-id ID] [--app-name NAME]
//                      [--mic] [--tone GAIN] [--device ID] [--list]
//
// Each line: backend, current device, period, output / input latency, and
// the stream stats (callbacks, xruns, slowest callback). Device events
// (hotplug, default changes, our stream moving) are printed as they arrive.
// Exit status 0; the last line is a machine-readable summary.

#include "broaudio/engine.h"
#include "broaudio/device.h"
#include "broaudio/log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace broaudio;

int main(int argc, char** argv)
{
    int seconds = 10;
    bool mic = false, list = false;
    float tone = 0.05f;
    AudioDeviceConfig cfg;
    cfg.appId = "org.broaudio.probe";
    cfg.appName = "broaudio probe";
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--seconds")) seconds = std::atoi(next());
        else if (!std::strcmp(argv[i], "--backend")) parseAudioBackendKind(next(), &cfg.backend);
        else if (!std::strcmp(argv[i], "--period")) cfg.periodFrames = std::atoi(next());
        else if (!std::strcmp(argv[i], "--app-id")) cfg.appId = next();
        else if (!std::strcmp(argv[i], "--app-name")) cfg.appName = next();
        else if (!std::strcmp(argv[i], "--device")) cfg.outputDevice = next();
        else if (!std::strcmp(argv[i], "--tone")) tone = static_cast<float>(std::atof(next()));
        else if (!std::strcmp(argv[i], "--mic")) mic = true;
        else if (!std::strcmp(argv[i], "--list")) list = true;
    }

    setLogCallback([](LogLevel, const char* msg) { std::printf("[log] %s\n", msg); std::fflush(stdout); });

    Engine e;
    if (!e.init(cfg)) {
        std::printf("init failed\n");
        return 1;
    }
    e.setAudioDeviceEventCallback([](const AudioDeviceEvent& ev) {
        static const char* const k[] = {"added", "removed", "default", "moved", "lost"};
        std::printf("[event] %s %s %s %s\n", k[static_cast<int>(ev.kind)],
                    ev.direction == AudioDirection::Playback ? "playback" : "capture",
                    ev.deviceId.c_str(), ev.name.c_str());
        std::fflush(stdout);
    });

    if (list) {
        for (AudioDirection d : {AudioDirection::Playback, AudioDirection::Capture})
            for (auto& dev : e.audioDevices(d))
                std::printf("[device] %s %s%s \"%s\" %dch %dHz\n",
                            d == AudioDirection::Playback ? "playback" : "capture",
                            dev.isDefault ? "* " : "", dev.id.c_str(), dev.name.c_str(),
                            dev.channels, dev.sampleRate);
    }

    if (tone > 0.0f) {
        int v = e.createVoice();
        e.setFrequency(v, 440.0f);
        e.setGain(v, tone);
        e.setVoicePersistent(v, true);
        e.startVoice(v, 0.0);
    }
    if (mic && !e.startMicCapture()) std::printf("mic failed\n");

    const auto t0 = std::chrono::steady_clock::now();
    double lastClock = e.currentTime();
    for (int s = 1; s <= seconds; ++s) {
        // update() delivers device events; tick it like a UI loop would.
        for (int k = 0; k < 20; ++k) {
            e.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        AudioStreamInfo oi = e.outputStreamInfo();
        AudioStreamStats os = e.outputStreamStats();
        AudioStreamInfo ii = e.inputStreamInfo();
        AudioStreamStats is = e.inputStreamStats();
        const double clock = e.currentTime();
        std::printf("t=%3ds backend=%s dev=\"%s\" period=%d out_lat=%.2fms cb=%llu xruns=%llu "
                    "maxcb=%.3fms clock+%.3fs",
                    s, e.audioBackendName(), oi.deviceName.c_str(), oi.periodFrames,
                    oi.latencySeconds * 1e3, static_cast<unsigned long long>(os.callbacks),
                    static_cast<unsigned long long>(os.xruns), os.maxCallbackSeconds * 1e3,
                    clock - lastClock);
        if (mic)
            std::printf(" | in_dev=\"%s\" in_period=%d in_lat=%.2fms in_cb=%llu in_xruns=%llu",
                        ii.deviceName.c_str(), ii.periodFrames, ii.latencySeconds * 1e3,
                        static_cast<unsigned long long>(is.callbacks),
                        static_cast<unsigned long long>(is.xruns));
        std::printf("\n");
        std::fflush(stdout);
        lastClock = clock;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    AudioStreamInfo oi = e.outputStreamInfo();
    AudioStreamStats os = e.outputStreamStats();
    AudioStreamInfo ii = e.inputStreamInfo();
    AudioStreamStats is = e.inputStreamStats();
    std::printf("SUMMARY backend=%s requested_period=%d actual_period=%d out_latency_ms=%.2f "
                "in_latency_ms=%.2f wall_s=%.1f audio_s=%.1f out_callbacks=%llu out_xruns=%llu "
                "in_xruns=%llu max_callback_ms=%.3f\n",
                e.audioBackendName(), cfg.periodFrames, oi.periodFrames, oi.latencySeconds * 1e3,
                ii.latencySeconds * 1e3, wall, e.currentTime(),
                static_cast<unsigned long long>(os.callbacks),
                static_cast<unsigned long long>(os.xruns),
                static_cast<unsigned long long>(is.xruns), os.maxCallbackSeconds * 1e3);
    e.shutdown();
    return 0;
}
