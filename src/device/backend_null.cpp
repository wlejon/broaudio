// Null backend: no device. A timer thread runs each stream's callback once per
// period at the requested rate; playback output is discarded and capture
// delivers silence. Lets tests exercise the realtime path (Engine::init, the
// mic stream, stats) on machines with no sound server or hardware.

#include "broaudio/device.h"
#include "backends.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace broaudio::detail {
namespace {

using Clock = std::chrono::steady_clock;

class NullStream final : public AudioStream {
public:
    NullStream(const AudioStreamConfig& cfg, AudioProcessFn fn, void* user)
        : cfg_(cfg), fn_(fn), user_(user),
          period_(std::clamp(cfg.periodFrames, 16, 8192)),
          buf_(static_cast<size_t>(period_) * static_cast<size_t>(cfg.channels), 0.0f) {}
    ~NullStream() override { stop(); }

    bool start() override
    {
        if (thread_.joinable()) return true;
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

    AudioStreamInfo info() const override
    {
        AudioStreamInfo i;
        i.sampleRate = cfg_.sampleRate;
        i.channels = cfg_.channels;
        i.periodFrames = period_;
        i.latencySeconds = static_cast<double>(period_) / cfg_.sampleRate;
        i.deviceName = "null";
        return i;
    }

    AudioStreamStats stats() const override
    {
        AudioStreamStats s;
        s.callbacks = callbacks_.load(std::memory_order_relaxed);
        s.frames = s.callbacks * static_cast<uint64_t>(period_);
        return s;
    }

private:
    void run()
    {
        const auto period = std::chrono::nanoseconds(
            static_cast<long long>(1e9 * period_ / cfg_.sampleRate));
        auto next = Clock::now();
        while (running_.load(std::memory_order_relaxed)) {
            if (cfg_.direction == AudioDirection::Capture)
                std::fill(buf_.begin(), buf_.end(), 0.0f);
            fn_(user_, buf_.data(), period_);
            callbacks_.fetch_add(1, std::memory_order_relaxed);
            next += period;
            // A stalled host (debugger, overloaded box) does not get a burst
            // of catch-up callbacks: the clock restarts from now.
            const auto now = Clock::now();
            if (next < now - period * 4) next = now;
            std::this_thread::sleep_until(next);
        }
    }

    AudioStreamConfig cfg_;
    AudioProcessFn fn_;
    void* user_;
    int period_;
    std::vector<float> buf_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> callbacks_{0};
};

class NullBackend final : public AudioBackend {
public:
    AudioBackendKind kind() const override { return AudioBackendKind::Null; }

    std::vector<AudioDeviceInfo> devices(AudioDirection direction) override
    {
        AudioDeviceInfo d;
        d.id = "null";
        d.name = direction == AudioDirection::Playback ? "Null Output" : "Null Input";
        d.direction = direction;
        d.isDefault = true;
        return {d};
    }

    std::unique_ptr<AudioStream> openStream(const AudioStreamConfig& config,
                                            AudioProcessFn process, void* user,
                                            std::string* error) override
    {
        if (!process || config.channels < 1 || config.channels > 8 || config.sampleRate <= 0) {
            if (error) *error = "invalid stream config";
            return nullptr;
        }
        return std::make_unique<NullStream>(config, process, user);
    }

    void pollEvents(const AudioDeviceEventFn&) override {}
};

} // namespace

std::unique_ptr<AudioBackend> createNullAudioBackend()
{
    return std::make_unique<NullBackend>();
}

} // namespace broaudio::detail
