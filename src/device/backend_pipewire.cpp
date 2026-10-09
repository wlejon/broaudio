// PipeWire backend: a native libpipewire client.
//
// One pw_thread_loop per backend runs the control plane (registry, default
// metadata, stream state). Streams are pw_streams created with
// PW_STREAM_FLAG_RT_PROCESS, so the process callback runs on PipeWire's
// realtime data thread and calls straight into the engine: no extra thread,
// no ring, no lock between the graph and the mixer.
//
// Each stream is a graph node named from the app id (node.name
// "<appId>.output" / "<appId>.input", application.name/id, media.name, an
// optional media.role) and asks for its period through node.latency. With no
// target it is left for the session manager to link to the default device,
// which also moves it when the default changes or the device it is on goes
// away; the backend watches its links to report where it ended up.
//
// Locking: the registry/metadata/state callbacks run on the loop thread and
// take stateMutex_ (control plane only). The data thread touches nothing but
// the stream's own atomics and the shared io_position the graph hands us.

#include "broaudio/device.h"
#include "broaudio/log.h"
#include "backends.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef PW_KEY_TARGET_OBJECT
#define PW_KEY_TARGET_OBJECT PW_KEY_NODE_TARGET   // before 0.3.64
#endif

namespace broaudio::detail {
namespace {

using Clock = std::chrono::steady_clock;

void ensurePwInit()
{
    // pw_init is safe to call repeatedly; pw_deinit is not safe while another
    // library in the process (bropulse) still uses PipeWire, so never call it.
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

// `{"name":"alsa_output..."}` -> alsa_output... (the default-device metadata
// values). Returns empty for null / anything without a "name" string.
std::string jsonName(const char* json)
{
    if (!json) return {};
    const char* k = std::strstr(json, "\"name\"");
    if (!k) return {};
    const char* q = std::strchr(k + 6, '"');
    if (!q) return {};
    std::string out;
    for (const char* p = q + 1; *p && *p != '"'; ++p) {
        if (*p == '\\' && p[1]) ++p;
        out.push_back(*p);
    }
    return out;
}

// node.name allows anything, but tools and WirePlumber rules match on it;
// keep it to the conventional [A-Za-z0-9._-].
std::string sanitizeNodeName(const std::string& s)
{
    std::string out;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        out.push_back(std::isalnum(u) || c == '.' || c == '_' || c == '-' ? c : '_');
    }
    return out.empty() ? std::string("broaudio") : out;
}

struct NodeRec {
    uint32_t id = 0;
    std::string name;   // node.name
    std::string desc;   // node.description (else nick, else name)
    AudioDirection dir = AudioDirection::Playback;
    int channels = 0;
    int rate = 0;
};

struct LinkRec {
    uint32_t outNode = 0;
    uint32_t inNode = 0;
};

class PwBackend;

class PwStream final : public AudioStream {
public:
    PwStream(PwBackend* b, const AudioStreamConfig& cfg, AudioProcessFn fn, void* user)
        : backend_(b), cfg_(cfg), fn_(fn), user_(user) {}
    ~PwStream() override;

    bool open(std::string* error);
    bool start() override;
    void stop() override;
    AudioStreamInfo info() const override;
    AudioStreamStats stats() const override;

    uint32_t nodeId() const { return nodeId_.load(std::memory_order_relaxed); }
    AudioDirection direction() const { return cfg_.direction; }
    // Control-plane bookkeeping for StreamMoved (backend's pollEvents only).
    std::string lastDevice;

private:
    static void onProcess(void* data);
    static void onIoChanged(void* data, uint32_t id, void* area, uint32_t size);
    static void onStateChanged(void* data, pw_stream_state old, pw_stream_state state,
                               const char* error);
    void syncWithDataThread();

    PwBackend* backend_;
    AudioStreamConfig cfg_;
    AudioProcessFn fn_;
    void* user_;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};

    std::atomic<bool> active_{false};
    std::atomic<uint32_t> nodeId_{SPA_ID_INVALID};
    std::atomic<spa_io_position*> position_{nullptr};

    // Data-thread state.
    uint64_t lastPos_ = 0;
    uint64_t lastDur_ = 0;
    uint32_t lastClockId_ = SPA_ID_INVALID;
    uint32_t lastCycle_ = 0;
    std::atomic<bool> resetClock_{true};

    // Data thread -> control plane (relaxed; diagnostics).
    std::atomic<uint64_t> callbacks_{0};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> xruns_{0};
    std::atomic<uint64_t> maxCallbackNs_{0};
    std::atomic<uint64_t> quantumDuration_{0};   // ticks at quantumRateDenom_ (num is 1)
    std::atomic<uint32_t> quantumRateDenom_{0};
};

class PwBackend final : public AudioBackend {
public:
    ~PwBackend() override;
    bool connect(std::string* error);

    AudioBackendKind kind() const override { return AudioBackendKind::PipeWire; }
    std::vector<AudioDeviceInfo> devices(AudioDirection direction) override;
    std::unique_ptr<AudioStream> openStream(const AudioStreamConfig& config,
                                            AudioProcessFn process, void* user,
                                            std::string* error) override;
    void pollEvents(const AudioDeviceEventFn& fn) override;

    void lock() { pw_thread_loop_lock(loop_); }
    void unlock() { pw_thread_loop_unlock(loop_); }
    pw_core* core() const { return core_; }
    pw_loop* dataLoop() const { return pw_data_loop_get_loop(pw_context_get_data_loop(context_)); }

    void forget(PwStream* s)
    {
        std::lock_guard<std::mutex> g(stateMutex_);
        streams_.erase(std::remove(streams_.begin(), streams_.end(), s), streams_.end());
    }
    void queueEvent(AudioDeviceEvent ev)
    {
        std::lock_guard<std::mutex> g(eventsMutex_);
        pending_.push_back(std::move(ev));
    }
    // Device the stream's node is linked to now: node.name + description.
    std::pair<std::string, std::string> linkedDevice(const PwStream& s);

private:
    static void onGlobal(void* data, uint32_t id, uint32_t permissions, const char* type,
                         uint32_t version, const spa_dict* props);
    static void onGlobalRemove(void* data, uint32_t id);
    static int onMetadataProperty(void* data, uint32_t subject, const char* key,
                                  const char* type, const char* value);
    static void onCoreDone(void* data, uint32_t id, int seq);
    static void onCoreError(void* data, uint32_t id, int seq, int res, const char* message);

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_metadata* metadata_ = nullptr;
    spa_hook coreListener_{};
    spa_hook registryListener_{};
    spa_hook metadataListener_{};
    int syncSeq_ = 0;
    int doneSeq_ = -1;
    bool primed_ = false;      // initial registry dump done; later globals are hotplug

    std::mutex stateMutex_;
    std::map<uint32_t, NodeRec> devices_;
    std::map<uint32_t, LinkRec> links_;
    std::string defaultSink_, defaultSource_;
    std::vector<PwStream*> streams_;

    std::mutex eventsMutex_;
    std::vector<AudioDeviceEvent> pending_;
};

// ---------------------------------------------------------------------------
// PwBackend
// ---------------------------------------------------------------------------

void PwBackend::onGlobal(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
                         uint32_t /*version*/, const spa_dict* props)
{
    auto* self = static_cast<PwBackend*>(data);
    if (!props) return;
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!cls) return;
        NodeRec n;
        if (std::strncmp(cls, "Audio/Sink", 10) == 0) n.dir = AudioDirection::Playback;
        else if (std::strncmp(cls, "Audio/Source", 12) == 0) n.dir = AudioDirection::Capture;
        else return;
        n.id = id;
        n.name = str(spa_dict_lookup(props, PW_KEY_NODE_NAME));
        n.desc = str(spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION));
        if (n.desc.empty()) n.desc = str(spa_dict_lookup(props, PW_KEY_NODE_NICK));
        if (n.desc.empty()) n.desc = n.name;
        if (const char* ch = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNELS)) n.channels = std::atoi(ch);
        if (const char* r = spa_dict_lookup(props, PW_KEY_AUDIO_RATE)) n.rate = std::atoi(r);
        {
            std::lock_guard<std::mutex> g(self->stateMutex_);
            self->devices_[id] = n;
        }
        if (self->primed_)
            self->queueEvent({AudioDeviceEvent::Kind::Added, n.dir, n.name, n.desc});
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        const char* o = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
        const char* i = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
        if (!o || !i) return;
        std::lock_guard<std::mutex> g(self->stateMutex_);
        self->links_[id] = {static_cast<uint32_t>(std::strtoul(o, nullptr, 10)),
                            static_cast<uint32_t>(std::strtoul(i, nullptr, 10))};
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* mname = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!mname || std::strcmp(mname, "default") != 0 || self->metadata_) return;
        self->metadata_ = static_cast<pw_metadata*>(
            pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
        if (!self->metadata_) return;
        static const pw_metadata_events kEvents = [] {
            pw_metadata_events e{};
            e.version = PW_VERSION_METADATA_EVENTS;
            e.property = &PwBackend::onMetadataProperty;
            return e;
        }();
        pw_metadata_add_listener(self->metadata_, &self->metadataListener_, &kEvents, self);
    }
}

void PwBackend::onGlobalRemove(void* data, uint32_t id)
{
    auto* self = static_cast<PwBackend*>(data);
    NodeRec gone;
    bool wasDevice = false;
    {
        std::lock_guard<std::mutex> g(self->stateMutex_);
        self->links_.erase(id);
        auto it = self->devices_.find(id);
        if (it != self->devices_.end()) {
            gone = it->second;
            wasDevice = true;
            self->devices_.erase(it);
        }
    }
    if (wasDevice)
        self->queueEvent({AudioDeviceEvent::Kind::Removed, gone.dir, gone.name, gone.desc});
}

int PwBackend::onMetadataProperty(void* data, uint32_t subject, const char* key,
                                  const char* /*type*/, const char* value)
{
    auto* self = static_cast<PwBackend*>(data);
    if (subject != PW_ID_CORE) return 0;
    const bool clearAll = key == nullptr;
    const bool sink = clearAll || std::strcmp(key, "default.audio.sink") == 0;
    const bool source = clearAll || std::strcmp(key, "default.audio.source") == 0;
    if (!sink && !source) return 0;
    std::string name = jsonName(value);
    std::vector<AudioDeviceEvent> evs;
    {
        std::lock_guard<std::mutex> g(self->stateMutex_);
        if (sink && name != self->defaultSink_) {
            self->defaultSink_ = name;
            if (!name.empty()) evs.push_back({AudioDeviceEvent::Kind::DefaultChanged,
                                              AudioDirection::Playback, name, {}});
        }
        if (source && name != self->defaultSource_) {
            self->defaultSource_ = name;
            if (!name.empty()) evs.push_back({AudioDeviceEvent::Kind::DefaultChanged,
                                              AudioDirection::Capture, name, {}});
        }
    }
    if (self->primed_)
        for (auto& e : evs) self->queueEvent(std::move(e));
    return 0;
}

void PwBackend::onCoreDone(void* data, uint32_t id, int seq)
{
    auto* self = static_cast<PwBackend*>(data);
    if (id == PW_ID_CORE) {
        self->doneSeq_ = seq;
        pw_thread_loop_signal(self->loop_, false);
    }
}

void PwBackend::onCoreError(void* data, uint32_t id, int /*seq*/, int res, const char* message)
{
    auto* self = static_cast<PwBackend*>(data);
    log(LogLevel::Warn, "broaudio: PipeWire error on object %u: %s (%s)", id,
        message ? message : "", spa_strerror(res));
    if (id == PW_ID_CORE && res == -EPIPE) {
        // The daemon went away: every stream is dead.
        std::vector<PwStream*> streams;
        {
            std::lock_guard<std::mutex> g(self->stateMutex_);
            streams = self->streams_;
        }
        for (PwStream* s : streams)
            self->queueEvent({AudioDeviceEvent::Kind::StreamLost, s->direction(), {}, {}});
    }
    pw_thread_loop_signal(self->loop_, false);
}

bool PwBackend::connect(std::string* error)
{
    ensurePwInit();
    loop_ = pw_thread_loop_new("broaudio-pw", nullptr);
    if (!loop_) {
        if (error) *error = "pw_thread_loop_new failed";
        return false;
    }
    // The kernel SIGKILLs a process whose realtime thread runs past
    // RLIMIT_RTTIME without blocking, and an unprivileged process cannot raise
    // a lowered hard limit. module-rt lowers it as each context loads, to what
    // rtkit or xdg-desktop-portal's Realtime interface allows, and a portal
    // started before rtkit says 0: the first tick that lands while the data
    // thread is processing would kill the whole app. Below a sane budget the
    // data loop runs without realtime (loop.rt-prio 0) instead.
    auto rtBudgetTooSmall = [] {
        rlimit rl{};
        if (getrlimit(RLIMIT_RTTIME, &rl) != 0) return false;
        return rl.rlim_max != RLIM_INFINITY && rl.rlim_max < 10000;
    };
    auto makeContext = [&](bool rt) {
        pw_properties* props = rt ? nullptr : pw_properties_new(PW_KEY_LOOP_RT_PRIO, "0", nullptr);
        return pw_context_new(pw_thread_loop_get_loop(loop_), props, 0);
    };
    bool rt = !rtBudgetTooSmall();
    context_ = makeContext(rt);
    if (context_ && rt && rtBudgetTooSmall()) {
        pw_context_destroy(context_);
        rt = false;
        context_ = makeContext(false);
    }
    if (!rt) {
        rlimit rl{};
        getrlimit(RLIMIT_RTTIME, &rl);
        log(LogLevel::Warn,
            "broaudio: PipeWire audio runs without realtime scheduling: RLIMIT_RTTIME is %llu us "
            "(a portal started before rtkit says 0: restart xdg-desktop-portal)",
            static_cast<unsigned long long>(rl.rlim_max));
    }
    if (!context_) {
        if (error) *error = "pw_context_new failed";
        return false;
    }
    if (pw_thread_loop_start(loop_) < 0) {
        if (error) *error = "pw_thread_loop_start failed";
        return false;
    }

    lock();
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        unlock();
        if (error) *error = "no PipeWire daemon reachable";
        return false;
    }
    static const pw_core_events kCoreEvents = [] {
        pw_core_events e{};
        e.version = PW_VERSION_CORE_EVENTS;
        e.done = &PwBackend::onCoreDone;
        e.error = &PwBackend::onCoreError;
        return e;
    }();
    pw_core_add_listener(core_, &coreListener_, &kCoreEvents, this);

    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    static const pw_registry_events kRegistryEvents = [] {
        pw_registry_events e{};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = &PwBackend::onGlobal;
        e.global_remove = &PwBackend::onGlobalRemove;
        return e;
    }();
    pw_registry_add_listener(registry_, &registryListener_, &kRegistryEvents, this);

    // Two round trips: the first delivers the globals (and binds the default
    // metadata), the second the metadata's current properties.
    for (int round = 0; round < 2; ++round) {
        syncSeq_ = pw_core_sync(core_, PW_ID_CORE, syncSeq_);
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (doneSeq_ != syncSeq_) {
            if (Clock::now() > deadline) {
                unlock();
                if (error) *error = "PipeWire daemon did not answer";
                return false;
            }
            pw_thread_loop_timed_wait(loop_, 1);
        }
    }
    primed_ = true;
    unlock();
    return true;
}

PwBackend::~PwBackend()
{
    if (loop_) {
        lock();
        if (metadata_) {
            spa_hook_remove(&metadataListener_);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata_));
        }
        if (registry_) {
            spa_hook_remove(&registryListener_);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        }
        if (core_) {
            spa_hook_remove(&coreListener_);
            pw_core_disconnect(core_);
        }
        unlock();
        pw_thread_loop_stop(loop_);
    }
    if (context_) pw_context_destroy(context_);
    if (loop_) pw_thread_loop_destroy(loop_);
}

std::vector<AudioDeviceInfo> PwBackend::devices(AudioDirection direction)
{
    std::vector<AudioDeviceInfo> out;
    std::lock_guard<std::mutex> g(stateMutex_);
    const std::string& def = direction == AudioDirection::Playback ? defaultSink_ : defaultSource_;
    for (auto& [id, n] : devices_) {
        if (n.dir != direction) continue;
        AudioDeviceInfo d;
        d.id = n.name;
        d.name = n.desc;
        d.direction = n.dir;
        d.isDefault = !def.empty() && n.name == def;
        d.channels = n.channels;
        d.sampleRate = n.rate;
        out.push_back(std::move(d));
    }
    return out;
}

std::pair<std::string, std::string> PwBackend::linkedDevice(const PwStream& s)
{
    const uint32_t node = s.nodeId();
    if (node == SPA_ID_INVALID) return {};
    std::lock_guard<std::mutex> g(stateMutex_);
    for (auto& [id, l] : links_) {
        uint32_t peer = SPA_ID_INVALID;
        if (s.direction() == AudioDirection::Playback && l.outNode == node) peer = l.inNode;
        if (s.direction() == AudioDirection::Capture && l.inNode == node) peer = l.outNode;
        if (peer == SPA_ID_INVALID) continue;
        auto it = devices_.find(peer);
        if (it != devices_.end()) return {it->second.name, it->second.desc};
    }
    return {};
}

std::unique_ptr<AudioStream> PwBackend::openStream(const AudioStreamConfig& config,
                                                   AudioProcessFn process, void* user,
                                                   std::string* error)
{
    if (!process || config.channels < 1 || config.channels > 8 || config.sampleRate <= 0) {
        if (error) *error = "invalid stream config";
        return nullptr;
    }
    auto s = std::make_unique<PwStream>(this, config, process, user);
    if (!s->open(error)) return nullptr;
    std::lock_guard<std::mutex> g(stateMutex_);
    streams_.push_back(s.get());
    return s;
}

void PwBackend::pollEvents(const AudioDeviceEventFn& fn)
{
    std::vector<AudioDeviceEvent> evs;
    {
        std::lock_guard<std::mutex> g(eventsMutex_);
        evs.swap(pending_);
    }
    std::vector<PwStream*> streams;
    {
        std::lock_guard<std::mutex> g(stateMutex_);
        streams = streams_;
    }
    for (PwStream* s : streams) {
        auto [dev, desc] = linkedDevice(*s);
        if (dev.empty() || dev == s->lastDevice) continue;
        const bool first = s->lastDevice.empty();
        s->lastDevice = dev;
        if (!first)
            evs.push_back({AudioDeviceEvent::Kind::StreamMoved, s->direction(), dev, desc});
    }
    if (fn)
        for (auto& e : evs) fn(e);
}

// ---------------------------------------------------------------------------
// PwStream
// ---------------------------------------------------------------------------

bool PwStream::open(std::string* error)
{
    const bool playback = cfg_.direction == AudioDirection::Playback;
    const std::string appId = cfg_.appId.empty() ? std::string("broaudio") : cfg_.appId;
    const std::string appName = cfg_.appName.empty() ? appId : cfg_.appName;
    const std::string streamName = !cfg_.streamName.empty() ? cfg_.streamName
                                   : playback ? std::string("Output") : std::string("Input");
    const std::string nodeName = sanitizeNodeName(appId) + (playback ? ".output" : ".input");
    const int period = std::clamp(cfg_.periodFrames, 16, 8192);

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, playback ? "Playback" : "Capture",
        PW_KEY_APP_NAME, appName.c_str(),
        PW_KEY_APP_ID, appId.c_str(),
        PW_KEY_NODE_NAME, nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, appName.c_str(),
        PW_KEY_MEDIA_NAME, streamName.c_str(),
        nullptr);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", period, cfg_.sampleRate);
    if (!cfg_.role.empty()) pw_properties_set(props, PW_KEY_MEDIA_ROLE, cfg_.role.c_str());
    if (!cfg_.deviceId.empty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, cfg_.deviceId.c_str());

    spa_audio_info_raw info;
    std::memset(&info, 0, sizeof(info));
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = static_cast<uint32_t>(cfg_.sampleRate);
    info.channels = static_cast<uint32_t>(cfg_.channels);
    if (cfg_.channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (cfg_.channels == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.flags |= SPA_AUDIO_FLAG_UNPOSITIONED;
    }
    uint8_t podBuf[1024];
    spa_pod_builder b;
    spa_pod_builder_init(&b, podBuf, sizeof(podBuf));
    const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

    static const pw_stream_events kEvents = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = &PwStream::onStateChanged;
        e.io_changed = &PwStream::onIoChanged;
        e.process = &PwStream::onProcess;
        return e;
    }();

    backend_->lock();
    stream_ = pw_stream_new(backend_->core(), streamName.c_str(), props);  // takes props
    if (!stream_) {
        backend_->unlock();
        if (error) *error = "pw_stream_new failed";
        return false;
    }
    pw_stream_add_listener(stream_, &listener_, &kEvents, this);
    const auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE);
    int res = pw_stream_connect(stream_, playback ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
                                PW_ID_ANY, flags, params, 1);
    backend_->unlock();
    if (res < 0) {
        if (error) *error = std::string("pw_stream_connect: ") + spa_strerror(res);
        return false;
    }
    return true;
}

PwStream::~PwStream()
{
    stop();
    if (stream_) {
        backend_->lock();
        spa_hook_remove(&listener_);
        pw_stream_destroy(stream_);
        backend_->unlock();
        stream_ = nullptr;
    }
    backend_->forget(this);
}

bool PwStream::start()
{
    if (!stream_) return false;
    resetClock_.store(true, std::memory_order_relaxed);
    active_.store(true, std::memory_order_release);
    backend_->lock();
    int res = pw_stream_set_active(stream_, true);
    backend_->unlock();
    return res >= 0;
}

void PwStream::syncWithDataThread()
{
    // A blocking no-op on the data loop: once it returns, any process()
    // that was in flight has finished.
    pw_loop* dl = nullptr;
#if PW_CHECK_VERSION(1, 1, 0)
    dl = pw_stream_get_data_loop(stream_);
#endif
    if (!dl) dl = backend_->dataLoop();
    if (dl)
        pw_loop_invoke(dl, [](spa_loop*, bool, uint32_t, const void*, size_t, void*) { return 0; },
                       0, nullptr, 0, true, nullptr);
}

void PwStream::stop()
{
    if (!stream_ || !active_.exchange(false, std::memory_order_acq_rel)) return;
    backend_->lock();
    pw_stream_set_active(stream_, false);
    backend_->unlock();
    syncWithDataThread();
}

void PwStream::onIoChanged(void* data, uint32_t id, void* area, uint32_t /*size*/)
{
    auto* self = static_cast<PwStream*>(data);
    if (id == SPA_IO_Position)
        self->position_.store(static_cast<spa_io_position*>(area), std::memory_order_release);
}

void PwStream::onStateChanged(void* data, pw_stream_state /*old*/, pw_stream_state state,
                              const char* error)
{
    auto* self = static_cast<PwStream*>(data);
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
        self->nodeId_.store(pw_stream_get_node_id(self->stream_), std::memory_order_relaxed);
    if (state == PW_STREAM_STATE_ERROR) {
        log(LogLevel::Warn, "broaudio: PipeWire stream error: %s", error ? error : "");
        self->backend_->queueEvent({AudioDeviceEvent::Kind::StreamLost, self->cfg_.direction, {}, {}});
    }
}

void PwStream::onProcess(void* data)
{
    auto* self = static_cast<PwStream*>(data);
    const auto t0 = Clock::now();

    // Missed cycles. XRUN_RECOVER is set on the cycle after this node blew
    // its deadline; a position that is not where the last cycle ended means
    // the driver skipped ahead (its own xrun). A new clock id (the stream
    // moved to another driver) or a fresh start resets the reference.
    if (spa_io_position* pos = self->position_.load(std::memory_order_acquire)) {
        const spa_io_clock& c = pos->clock;
        if (self->resetClock_.exchange(false, std::memory_order_relaxed)
            || c.id != self->lastClockId_ || c.cycle != self->lastCycle_) {
            self->lastClockId_ = c.id;
            self->lastCycle_ = c.cycle;
        } else {
            if (c.flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER)
                self->xruns_.fetch_add(1, std::memory_order_relaxed);
            else if (self->lastDur_ && c.position != self->lastPos_ + self->lastDur_)
                self->xruns_.fetch_add(1, std::memory_order_relaxed);
        }
        self->lastPos_ = c.position;
        self->lastDur_ = c.duration;
        self->quantumDuration_.store(c.duration, std::memory_order_relaxed);
        self->quantumRateDenom_.store(c.rate.num ? c.rate.denom / c.rate.num : 0,
                                      std::memory_order_relaxed);
    }

    pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
    if (!b) {
        self->xruns_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    spa_data& d = b->buffer->datas[0];
    const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * static_cast<uint32_t>(self->cfg_.channels);
    uint32_t n = 0;
    if (d.data) {
        const bool active = self->active_.load(std::memory_order_acquire);
        if (self->cfg_.direction == AudioDirection::Playback) {
            n = d.maxsize / stride;
            if (b->requested) n = std::min<uint32_t>(n, static_cast<uint32_t>(b->requested));
            auto* dst = static_cast<float*>(d.data);
            if (active && n) self->fn_(self->user_, dst, static_cast<int>(n));
            else std::memset(dst, 0, static_cast<size_t>(n) * stride);
            d.chunk->offset = 0;
            d.chunk->stride = static_cast<int32_t>(stride);
            d.chunk->size = n * stride;
        } else {
            const uint32_t off = std::min(d.chunk->offset, d.maxsize);
            const uint32_t size = std::min(d.chunk->size, d.maxsize - off);
            n = size / stride;
            auto* src = reinterpret_cast<float*>(static_cast<uint8_t*>(d.data) + off);
            if (active && n) self->fn_(self->user_, src, static_cast<int>(n));
        }
    }
    pw_stream_queue_buffer(self->stream_, b);

    self->callbacks_.fetch_add(1, std::memory_order_relaxed);
    self->frames_.fetch_add(n, std::memory_order_relaxed);
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    if (ns > self->maxCallbackNs_.load(std::memory_order_relaxed))
        self->maxCallbackNs_.store(ns, std::memory_order_relaxed);
}

AudioStreamInfo PwStream::info() const
{
    AudioStreamInfo out;
    out.sampleRate = cfg_.sampleRate;
    out.channels = cfg_.channels;

    const uint64_t dur = quantumDuration_.load(std::memory_order_relaxed);
    const uint32_t graphRate = quantumRateDenom_.load(std::memory_order_relaxed);
    double periodSec = 0.0;
    if (dur && graphRate) {
        periodSec = static_cast<double>(dur) / graphRate;
        out.periodFrames = static_cast<int>(periodSec * cfg_.sampleRate + 0.5);
    }

    pw_time t;
    std::memset(&t, 0, sizeof(t));
    if (stream_ && pw_stream_get_time_n(stream_, &t, sizeof(t)) == 0 && t.rate.denom) {
        double delaySec = static_cast<double>(t.delay) * t.rate.num / t.rate.denom;
        if (delaySec < 0) delaySec = 0;
        const double stride = sizeof(float) * cfg_.channels;
        const double bufferedSec = (static_cast<double>(t.buffered) +
                                    static_cast<double>(t.queued) / stride) / cfg_.sampleRate;
        out.latencySeconds = periodSec + delaySec + bufferedSec;
    } else if (periodSec > 0) {
        out.latencySeconds = periodSec;
    }

    auto dev = backend_->linkedDevice(*this);
    out.deviceName = dev.second;
    return out;
}

AudioStreamStats PwStream::stats() const
{
    AudioStreamStats s;
    s.callbacks = callbacks_.load(std::memory_order_relaxed);
    s.frames = frames_.load(std::memory_order_relaxed);
    s.xruns = xruns_.load(std::memory_order_relaxed);
    s.maxCallbackSeconds = static_cast<double>(maxCallbackNs_.load(std::memory_order_relaxed)) * 1e-9;
    return s;
}

} // namespace

std::unique_ptr<AudioBackend> createPipeWireAudioBackend(std::string* error)
{
    auto b = std::make_unique<PwBackend>();
    if (!b->connect(error)) return nullptr;
    return b;
}

} // namespace broaudio::detail
