// SDL3 audio backend: one SDL_AudioStream bound to a device per AudioStream.
// SDL converts format, channels and rate between the callback and the device,
// and migrates streams opened on the default device when the default changes.
// SDL has no device-event thread we can hook without pumping SDL's event
// queue (which belongs to the host's window system), so hotplug is detected
// by diffing the device lists in pollEvents.

#include "broaudio/device.h"
#include "broaudio/log.h"
#include "backends.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace broaudio::detail {
namespace {

using Clock = std::chrono::steady_clock;

// Callback scratch, in frames. SDL asks for at most its device buffer plus
// conversion slack per callback; anything larger is processed in slices.
constexpr int kScratchFrames = 8192;

class SdlBackend;

class SdlStream final : public AudioStream {
public:
    SdlStream(SdlBackend* backend, const AudioStreamConfig& cfg, AudioProcessFn fn, void* user)
        : backend_(backend), cfg_(cfg), fn_(fn), user_(user),
          scratch_(static_cast<size_t>(kScratchFrames) * static_cast<size_t>(cfg.channels), 0.0f) {}
    ~SdlStream() override;

    bool open(std::string* error)
    {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = cfg_.channels;
        spec.freq = cfg_.sampleRate;

        SDL_AudioDeviceID target = cfg_.direction == AudioDirection::Playback
            ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK : SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
        if (!cfg_.deviceId.empty()) {
            char* end = nullptr;
            unsigned long id = std::strtoul(cfg_.deviceId.c_str(), &end, 10);
            if (!end || *end != '\0' || id == 0) {
                if (error) *error = "unknown SDL audio device id '" + cfg_.deviceId + "'";
                return false;
            }
            target = static_cast<SDL_AudioDeviceID>(id);
            specificDevice_ = target;
        }

        // SDL reads these when the device opens. The sample-frames hint is
        // process-wide and every open sets it, so it always carries this
        // stream's request.
        std::string frames = std::to_string(std::max(cfg_.periodFrames, 16));
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames.c_str());
        if (!cfg_.appName.empty() && !SDL_GetHint(SDL_HINT_APP_NAME))
            SDL_SetHint(SDL_HINT_APP_NAME, cfg_.appName.c_str());
        if (!cfg_.streamName.empty())
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, cfg_.streamName.c_str());
        if (!cfg_.role.empty())
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, cfg_.role.c_str());

        stream_ = SDL_OpenAudioDeviceStream(target, &spec, &SdlStream::callback, this);
        if (!stream_) {
            if (error) *error = SDL_GetError();
            return false;
        }
        return true;
    }

    bool start() override
    {
        if (!stream_) return false;
        return SDL_ResumeAudioStreamDevice(stream_);
    }

    void stop() override
    {
        // SDL holds the device lock across the callback, so once the pause
        // returns the callback is neither running nor going to run.
        if (stream_) SDL_PauseAudioStreamDevice(stream_);
    }

    AudioStreamInfo info() const override
    {
        AudioStreamInfo out;
        out.sampleRate = cfg_.sampleRate;
        out.channels = cfg_.channels;
        if (!stream_) return out;
        SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream_);
        SDL_AudioSpec devSpec{};
        int devFrames = 0;
        if (dev != 0 && SDL_GetAudioDeviceFormat(dev, &devSpec, &devFrames)
            && devFrames > 0 && devSpec.freq > 0) {
            // Only SDL's device buffer is visible from here: a lower bound.
            out.latencySeconds = static_cast<double>(devFrames) / static_cast<double>(devSpec.freq);
            out.periodFrames = static_cast<int>(
                static_cast<double>(devFrames) * cfg_.sampleRate / devSpec.freq + 0.5);
        }
        if (dev != 0) {
            if (const char* n = SDL_GetAudioDeviceName(dev)) out.deviceName = n;
        }
        return out;
    }

    AudioStreamStats stats() const override
    {
        AudioStreamStats s;
        s.callbacks = callbacks_.load(std::memory_order_relaxed);
        s.frames = frames_.load(std::memory_order_relaxed);
        s.maxCallbackSeconds = static_cast<double>(maxCallbackNs_.load(std::memory_order_relaxed)) * 1e-9;
        return s;
    }

    SDL_AudioDeviceID specificDevice() const { return specificDevice_; }
    AudioDirection direction() const { return cfg_.direction; }
    bool lostReported = false;   // backend's pollEvents bookkeeping

private:
    static void SDLCALL callback(void* userdata, SDL_AudioStream* stream,
                                 int additionalAmount, int /*totalAmount*/)
    {
        auto* self = static_cast<SdlStream*>(userdata);
        const auto t0 = Clock::now();
        const int ch = self->cfg_.channels;
        const int frameBytes = ch * static_cast<int>(sizeof(float));
        float* buf = self->scratch_.data();
        int done = 0;

        if (self->cfg_.direction == AudioDirection::Playback) {
            int frames = additionalAmount / frameBytes;
            while (frames > 0) {
                int chunk = std::min(frames, kScratchFrames);
                self->fn_(self->user_, buf, chunk);
                SDL_PutAudioStreamData(stream, buf, chunk * frameBytes);
                frames -= chunk;
                done += chunk;
            }
        } else {
            int avail = SDL_GetAudioStreamAvailable(stream) / frameBytes;
            while (avail > 0) {
                int chunk = std::min(avail, kScratchFrames);
                int got = SDL_GetAudioStreamData(stream, buf, chunk * frameBytes);
                if (got <= 0) break;
                int gotFrames = got / frameBytes;
                if (gotFrames <= 0) break;
                self->fn_(self->user_, buf, gotFrames);
                avail -= gotFrames;
                done += gotFrames;
            }
        }

        if (done > 0) {
            self->callbacks_.fetch_add(1, std::memory_order_relaxed);
            self->frames_.fetch_add(static_cast<uint64_t>(done), std::memory_order_relaxed);
        }
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        if (ns > self->maxCallbackNs_.load(std::memory_order_relaxed))
            self->maxCallbackNs_.store(ns, std::memory_order_relaxed);
    }

    SdlBackend* backend_;
    AudioStreamConfig cfg_;
    AudioProcessFn fn_;
    void* user_;
    std::vector<float> scratch_;
    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioDeviceID specificDevice_ = 0;
    std::atomic<uint64_t> callbacks_{0};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> maxCallbackNs_{0};
};

class SdlBackend final : public AudioBackend {
public:
    ~SdlBackend() override { SDL_QuitSubSystem(SDL_INIT_AUDIO); }

    AudioBackendKind kind() const override { return AudioBackendKind::Sdl; }

    std::vector<AudioDeviceInfo> devices(AudioDirection direction) override
    {
        std::vector<AudioDeviceInfo> out;
        int n = 0;
        SDL_AudioDeviceID* ids = direction == AudioDirection::Playback
            ? SDL_GetAudioPlaybackDevices(&n) : SDL_GetAudioRecordingDevices(&n);
        if (!ids) return out;
        for (int i = 0; i < n; ++i) {
            AudioDeviceInfo d;
            d.id = std::to_string(ids[i]);
            if (const char* name = SDL_GetAudioDeviceName(ids[i])) d.name = name;
            d.direction = direction;
            SDL_AudioSpec spec{};
            int frames = 0;
            if (SDL_GetAudioDeviceFormat(ids[i], &spec, &frames)) {
                d.channels = spec.channels;
                d.sampleRate = spec.freq;
            }
            out.push_back(std::move(d));
        }
        SDL_free(ids);
        return out;
    }

    std::unique_ptr<AudioStream> openStream(const AudioStreamConfig& config,
                                            AudioProcessFn process, void* user,
                                            std::string* error) override
    {
        if (!process || config.channels < 1 || config.channels > 8 || config.sampleRate <= 0) {
            if (error) *error = "invalid stream config";
            return nullptr;
        }
        auto s = std::make_unique<SdlStream>(this, config, process, user);
        if (!s->open(error)) return nullptr;
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streams_.push_back(s.get());
        return s;
    }

    void forget(SdlStream* s)
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streams_.erase(std::remove(streams_.begin(), streams_.end(), s), streams_.end());
    }

    void pollEvents(const AudioDeviceEventFn& fn) override
    {
        const auto now = Clock::now();
        if (primed_ && now - lastPoll_ < std::chrono::milliseconds(500)) return;
        lastPoll_ = now;
        diff(AudioDirection::Playback, known_[0], fn);
        diff(AudioDirection::Capture, known_[1], fn);
        primed_ = true;

        // A stream opened on a specific device whose device vanished is a
        // zombie in SDL (it plays into nothing). Default-device streams are
        // migrated by SDL and never land here.
        std::lock_guard<std::mutex> lock(streamsMutex_);
        for (SdlStream* s : streams_) {
            if (!s->specificDevice() || s->lostReported) continue;
            auto& set = known_[s->direction() == AudioDirection::Playback ? 0 : 1];
            if (set.count(std::to_string(s->specificDevice())) == 0) {
                s->lostReported = true;
                if (fn) fn({AudioDeviceEvent::Kind::StreamLost, s->direction(),
                            std::to_string(s->specificDevice()), {}});
            }
        }
    }

private:
    void diff(AudioDirection dir, std::set<std::string>& known, const AudioDeviceEventFn& fn)
    {
        auto list = devices(dir);
        std::set<std::string> now;
        for (auto& d : list) {
            now.insert(d.id);
            if (primed_ && !known.count(d.id) && fn)
                fn({AudioDeviceEvent::Kind::Added, dir, d.id, d.name});
        }
        if (primed_ && fn) {
            for (auto& id : known)
                if (!now.count(id)) fn({AudioDeviceEvent::Kind::Removed, dir, id, {}});
        }
        known.swap(now);
    }

    std::mutex streamsMutex_;
    std::vector<SdlStream*> streams_;
    std::set<std::string> known_[2];
    bool primed_ = false;
    Clock::time_point lastPoll_{};
};

SdlStream::~SdlStream()
{
    stop();
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    backend_->forget(this);
}

} // namespace

std::unique_ptr<AudioBackend> createSdlAudioBackend(std::string* error)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (error) *error = std::string("SDL audio init failed: ") + SDL_GetError();
        log(LogLevel::Error, "broaudio: Failed to init SDL audio: %s", SDL_GetError());
        return nullptr;
    }
    return std::make_unique<SdlBackend>();
}

} // namespace broaudio::detail
