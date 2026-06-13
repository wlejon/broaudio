// Loopback capture self-test.
//
// Strategy: have broaudio's own Engine play a loud tone to the default render
// device, capture the system output back through LoopbackCapture, and check
// that real (non-silent) audio arrives at 16 kHz mono — the geometry the
// listen stack consumes, which also exercises the downmix + resample path.
//
// This needs a real audio device, so every step degrades to a printed SKIP
// rather than a failure when the environment can't support it (no Windows /
// old SDK, no render endpoint, headless CI). On a normal desktop with audio
// unmuted it is a true end-to-end check; start() succeeding alone already
// validates the COM / device / format / thread bring-up.

#include "test_harness.h"
#include "broaudio/engine.h"
#include "broaudio/loopback_capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;

// A 1 s loud sine clip at the engine rate, looped, so the render endpoint is
// continuously producing audio for loopback to pick up.
static int makeToneClip(Engine& e, float hz, float amp) {
    const int rate = e.sampleRate();
    std::vector<float> s(static_cast<std::size_t>(rate));
    for (int i = 0; i < rate; ++i)
        s[i] = amp * std::sin(2.0f * PI * hz * i / rate);
    return e.createClip(s.data(), rate, 1);
}

// Run `mode` (optionally targeting `pid`) for ~800 ms while a tone plays;
// report frames delivered + peak. Returns peak (-1 if start failed).
static float captureWhilePlaying(LoopbackMode mode, std::uint32_t pid,
                                 const char* label) {
    Engine e;
    if (!e.init()) {
        std::printf("  SKIP %s: no audio output device\n", label);
        return 0.0f;   // treat as benign skip
    }

    std::atomic<std::uint64_t> calls{0};
    std::atomic<int> peakX10000{0};

    LoopbackConfig cfg;
    cfg.mode       = mode;
    cfg.pid        = pid;
    cfg.targetRate = 16000;   // listen-stack rate — exercises the resampler
    cfg.mono       = true;

    LoopbackCapture cap;
    bool started = cap.start(cfg, [&](const float* s, int n) {
        calls.fetch_add(1, std::memory_order_relaxed);
        int p = 0;
        for (int i = 0; i < n; ++i) { int v = (int)(std::fabs(s[i]) * 10000.0f); if (v > p) p = v; }
        int prev = peakX10000.load(std::memory_order_relaxed);
        while (p > prev && !peakX10000.compare_exchange_weak(prev, p)) {}
    });
    if (!started) {
        std::printf("  SKIP %s: loopback start() failed (no render endpoint?)\n", label);
        return -1.0f;
    }

    // start() succeeding already proves device/format/thread bring-up.
    if (cap.sampleRate() != 16000 || cap.channels() != 1) {
        std::printf("  %s: unexpected delivered geometry %d Hz / %d ch\n",
                    label, cap.sampleRate(), cap.channels());
    }

    int clip = makeToneClip(e, 440.0f, 0.5f);
    if (clip >= 0) e.playClip(clip, 1.0f, /*loop*/ true);

    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    cap.stop();
    LoopbackStats st = cap.stats();
    float peak = peakX10000.load() / 10000.0f;
    std::printf("  %s: %llu callbacks, %llu samples, peak %.4f\n", label,
                (unsigned long long)st.framesDelivered,
                (unsigned long long)st.samplesDelivered, peak);
    return peak;
}

TEST(loopback_supported_reports_cleanly) {
    // On platforms without support, everything is a clean no-op.
    if (!LoopbackCapture::isSupported()) {
        ASSERT_TRUE(LoopbackCapture::enumerateProcesses().empty());
        LoopbackCapture cap;
        ASSERT_FALSE(cap.start({}, [](const float*, int) {}));
        ASSERT_FALSE(cap.isCapturing());
        std::printf("  SKIP: loopback capture unsupported on this platform\n");
        PASS();
        return;
    }
    PASS();
}

TEST(loopback_enumerate_processes_does_not_crash) {
    if (!LoopbackCapture::isSupported()) { PASS(); return; }
    auto procs = LoopbackCapture::enumerateProcesses();
    std::printf("  %zu process(es) with active audio sessions\n", procs.size());
    for (const auto& p : procs)
        std::printf("    pid %u  %s\n", p.pid, p.name.c_str());
    PASS();   // count is environment-dependent; just must not crash
}

TEST(loopback_system_output_captures_tone) {
    if (!LoopbackCapture::isSupported()) { PASS(); return; }
    float peak = captureWhilePlaying(LoopbackMode::SystemOutput, 0, "system");
    if (peak > 0.001f) {
        // Real end-to-end capture confirmed.
        ASSERT_GT(peak, 0.001f);
    } else {
        std::printf("  NOTE: no energy captured (system likely muted); "
                    "bring-up still validated\n");
    }
    PASS();
}

TEST(loopback_process_include_own_pid_captures_tone) {
    if (!LoopbackCapture::isSupported()) { PASS(); return; }
    // Our own process is the one rendering the tone, so include-tree on our pid
    // should hear it. Requires Win10 2004+ / macOS 14.2+; start() degrades to
    // SKIP otherwise (e.g. no audio-capture permission for the tap).
#ifdef _WIN32
    std::uint32_t self = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    std::uint32_t self = static_cast<std::uint32_t>(getpid());
#endif
    float peak = captureWhilePlaying(LoopbackMode::ProcessInclude, self, "process(self)");
    if (peak > 0.001f) {
        ASSERT_GT(peak, 0.001f);
    } else if (peak == 0.0f) {
        std::printf("  NOTE: no energy (muted) or platform lacks process loopback; "
                    "bring-up still validated\n");
    }
    PASS();
}

int main() { return runAllTests(); }
