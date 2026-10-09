#pragma once

// Backend-neutral audio device I/O: the one place broaudio talks to the OS's
// audio devices. The Engine opens its playback stream and its mic stream
// through an AudioBackend; nothing else in broaudio names a device API, and no
// backend's headers reach this file.
//
// Backends (selectAudioBackend picks one; see AudioBackendKind):
//   * PipeWire — Linux, native libpipewire client. The default on Linux when a
//     PipeWire daemon is reachable. Streams are real graph nodes named from the
//     app id, ask the graph for their period through node.latency, follow the
//     default device as the session manager moves them, and report their
//     measured latency and missed cycles.
//   * SDL      — SDL3 audio streams. Windows and macOS, and the Linux fallback
//     when there is no PipeWire daemon (or libpipewire was not found at
//     build time). Default devices follow the OS default (SDL migrates them).
//   * Null     — no device: a timer thread runs the callback at the requested
//     period and rate, discarding playback and delivering silent capture. For
//     tests that need the realtime path without sound hardware.
//
// Threading:
//   * AudioProcessFn runs on the backend's realtime thread. It must not lock,
//     allocate, block or do I/O. It is never called concurrently with itself
//     for one stream.
//   * Everything else (devices, openStream, start/stop, info, pollEvents) is
//     control plane: call it from non-audio threads. A backend is not
//     internally serialised against concurrent control calls from several
//     threads; keep it to one control thread (the Engine does).
//   * Device events are queued by the backend and delivered from pollEvents(),
//     on the thread that calls it (the Engine calls it from update()).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace broaudio {

enum class AudioBackendKind {
    Auto,      // BROAUDIO_BACKEND env, then platform policy (see selectAudioBackend)
    PipeWire,
    Sdl,
    Null,
};

// "auto" / "pipewire" / "sdl" / "null".
const char* audioBackendKindName(AudioBackendKind kind);
// Parses the names above (case-insensitive; "pw" for pipewire, "dummy" and
// "none" for null). Returns false and leaves *out alone for anything else.
bool parseAudioBackendKind(const char* name, AudioBackendKind* out);

// True when this build has the backend compiled in. PipeWire is Linux-only and
// needs libpipewire-0.3 at build time; SDL and Null are always present.
bool audioBackendCompiled(AudioBackendKind kind);

enum class AudioDirection { Playback, Capture };

struct AudioDeviceInfo {
    // Stable for the device's lifetime within one backend: the PipeWire
    // node.name, or the SDL device id as a decimal string. Pass it back as
    // AudioStreamConfig::deviceId.
    std::string id;
    std::string name;           // human-readable description
    AudioDirection direction = AudioDirection::Playback;
    bool isDefault = false;     // the system default for its direction (SDL cannot tell: false)
    int channels = 0;           // 0 = unknown
    int sampleRate = 0;         // 0 = unknown
};

struct AudioStreamConfig {
    AudioDirection direction = AudioDirection::Playback;
    // Empty = the system default, following it when the default changes.
    // Otherwise an AudioDeviceInfo::id; if that device goes away the stream
    // is moved to the default by the session manager (PipeWire) or reported
    // lost (SDL).
    std::string deviceId;
    int sampleRate = 48000;     // the callback's rate; the backend converts to the device's
    int channels = 2;           // interleaved channels in the callback buffer (1 or 2)
    // Requested callback period in frames at sampleRate. A hint: PipeWire
    // turns it into node.latency (the graph runs at the smallest period any
    // client asks for, within the server's quantum limits), SDL into its
    // device sample-frames hint. info().periodFrames is what you got.
    int periodFrames = 128;
    // Identity, shown by the system mixer. appId is a desktop app id
    // ("org.example.Game" or a folder name); appName a display name. Both
    // optional: empty falls back to "broaudio".
    std::string appId;
    std::string appName;
    std::string streamName;     // e.g. "Output", "Microphone"
    std::string role;           // PipeWire media.role ("Game", "Music", "Communication", ...); empty = none
};

struct AudioStreamInfo {
    int sampleRate = 0;
    int channels = 0;
    // The period the backend actually runs the callback at, in frames at
    // sampleRate (PipeWire: the graph quantum, converted; SDL: the device
    // buffer). 0 before the first cycle when the backend only learns it then.
    int periodFrames = 0;
    // Best estimate of device latency in seconds. Playback: from the moment
    // a frame is written in the callback until it reaches the DAC. Capture:
    // from the ADC until the callback sees it. PipeWire: one period plus the
    // graph's measured delay to the device (filters, the device buffer,
    // resampler). SDL: the device buffer only (a lower bound). 0 = unknown.
    double latencySeconds = 0.0;
    std::string deviceName;     // the device the stream is on now (may change); empty = unknown
};

struct AudioStreamStats {
    std::uint64_t callbacks = 0;     // process callbacks run
    std::uint64_t frames = 0;        // frames passed through the callback
    // Cycles the stream missed: PipeWire counts cycles where the graph flags
    // this node as having overrun its deadline, plus graph-position jumps
    // (the driver skipped ahead) and cycles with no buffer to fill. SDL and
    // Null do not report any (0).
    std::uint64_t xruns = 0;
    double maxCallbackSeconds = 0.0; // the slowest callback, wall time
};

// Playback: fill `frames` interleaved frames of `channels` floats into
// `buffer`. Capture: read them. Runs on the backend's realtime thread (see
// the threading notes above).
using AudioProcessFn = void (*)(void* user, float* buffer, int frames);

struct AudioDeviceEvent {
    enum class Kind {
        Added,            // a device appeared
        Removed,          // a device went away
        DefaultChanged,   // the system default for `direction` is now `deviceId`
        StreamMoved,      // an open stream of ours is now on `deviceId` (default follow, unplug)
        StreamLost,       // an open stream of ours lost its device and stopped
    };
    Kind kind = Kind::Added;
    AudioDirection direction = AudioDirection::Playback;
    std::string deviceId;
    std::string name;
};
using AudioDeviceEventFn = std::function<void(const AudioDeviceEvent&)>;

class AudioStream {
public:
    virtual ~AudioStream() = default;
    // Start calling the process function. Idempotent.
    virtual bool start() = 0;
    // Stop calling it. After stop() returns, the process function is not
    // running and will not be called until start(). Idempotent. The
    // destructor stops.
    virtual void stop() = 0;
    virtual AudioStreamInfo info() const = 0;
    virtual AudioStreamStats stats() const = 0;
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual AudioBackendKind kind() const = 0;
    const char* name() const { return audioBackendKindName(kind()); }

    virtual std::vector<AudioDeviceInfo> devices(AudioDirection direction) = 0;

    // Opens a stream, stopped. Returns null (and *error when given) on
    // failure. The returned stream must not outlive the backend.
    virtual std::unique_ptr<AudioStream> openStream(const AudioStreamConfig& config,
                                                    AudioProcessFn process, void* user,
                                                    std::string* error = nullptr) = 0;

    // Delivers queued device events to `fn` on the calling thread. Cheap when
    // nothing happened. SDL detects hotplug by diffing its device lists here,
    // so call it periodically (Engine::update does).
    virtual void pollEvents(const AudioDeviceEventFn& fn) = 0;
};

// Creates a backend. Auto resolves, in order:
//   1. $BROAUDIO_BACKEND ("pipewire" / "sdl" / "null"), when set and valid;
//   2. $SDL_AUDIODRIVER set -> SDL (an explicit SDL driver request, e.g.
//      "dummy" in test harnesses, keeps meaning what it always meant);
//   3. Linux with PipeWire compiled in and a daemon reachable -> PipeWire;
//   4. SDL.
// An explicit kind that fails to come up (no daemon, not compiled) falls back
// to SDL with a warning, except Null, which cannot fail. Returns null only if
// SDL audio itself cannot initialise; *error says why.
std::unique_ptr<AudioBackend> createAudioBackend(AudioBackendKind kind = AudioBackendKind::Auto,
                                                 std::string* error = nullptr);

} // namespace broaudio
