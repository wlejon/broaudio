// Linux PulseAudio loopback capture.
//
// SystemOutput: record from the default sink's *monitor* source — PulseAudio's
// native "what is the machine playing" stream (the render-side mix). We let the
// server do the format conversion: we ask for FP32 mono (or stereo) at the
// target rate and PulseAudio downmixes + resamples for us, so there is no SDL
// resampler on this path.
//
// ProcessInclude / ProcessExclude: PulseAudio has no public API to capture a
// single application's audio without rerouting its streams through a null sink
// (which would alter what the user actually hears). So the process modes report
// unsupported and start() fails — the same graceful degradation the Windows
// path uses when the SDK lacks process-loopback. enumerateProcesses() still
// works (it lists the apps with active render sessions, for a picker).
//
// Threading: PulseAudio's threaded mainloop owns the capture thread. Every touch
// of the context/stream is serialized under its lock; the read callback runs on
// that mainloop thread — so, exactly like the WASAPI path, the consumer callback
// runs off the main thread and must do real-time-safe work only. start()/stop()
// are main-thread only and bracket the mainloop with its lock then join it.
//
// This works against a real PulseAudio server and against pipewire-pulse (the
// PipeWire compatibility shim), which is what most modern desktops actually run.

#include "broaudio/loopback_capture.h"
#include "broaudio/log.h"

#include <pulse/pulseaudio.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace broaudio {
namespace {

// Run a one-shot introspection operation to completion under the mainloop lock,
// then release it. The supplied callback must pa_threaded_mainloop_signal() when
// it has the data it needs (on the eol/final call). Caller holds the lock.
void waitOp(pa_threaded_mainloop* ml, pa_operation* op) {
    if (!op) return;
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
        pa_threaded_mainloop_wait(ml);
    pa_operation_unref(op);
}

// Pull a uint32 property (e.g. application.process.id) out of a proplist.
bool propU32(pa_proplist* pl, const char* key, std::uint32_t& out) {
    if (!pl) return false;
    const char* v = pa_proplist_gets(pl, key);
    if (!v || !*v) return false;
    char* end = nullptr;
    unsigned long n = std::strtoul(v, &end, 10);
    if (end == v) return false;
    out = static_cast<std::uint32_t>(n);
    return true;
}

} // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct LoopbackCapture::Impl {
    pa_threaded_mainloop* ml     = nullptr;
    pa_context*           ctx    = nullptr;
    pa_stream*            stream = nullptr;

    LoopbackCallback cb;
    LoopbackConfig   cfg;

    std::atomic<bool> running{false};

    // Delivered geometry (read by sampleRate()/channels()/stats()).
    std::atomic<int> deliveredRate{0};
    std::atomic<int> deliveredChannels{0};

    // Stats (written on the mainloop thread, read on main).
    std::atomic<std::uint64_t> framesDelivered{0};
    std::atomic<std::uint64_t> samplesDelivered{0};
    std::atomic<int>           rollingPeakX10000{0};

    // Scratch resolved during start() (mainloop thread, under lock).
    std::string monitorName;   // "<sink>.monitor" to record from
    int         nativeRate = 0;
    int         nativeCh   = 0;

    void deliver(const float* s, int n);
    void teardown();

    // PulseAudio C callbacks (mainloop thread, under the lock). Static so they
    // match the C function-pointer signatures; `ud` is the owning Impl.
    static void contextStateCb(pa_context*, void* ud);
    static void streamStateCb(pa_stream*, void* ud);
    static void streamReadCb(pa_stream*, size_t nbytes, void* ud);
    static void serverInfoCb(pa_context*, const pa_server_info*, void* ud);
    static void sinkInfoCb(pa_context*, const pa_sink_info*, int eol, void* ud);
};

void LoopbackCapture::Impl::deliver(const float* s, int n) {
    if (n <= 0) return;
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        float a = std::fabs(s[i]);
        if (a > peak) peak = a;
    }
    int pk   = static_cast<int>(peak * 10000.0f);
    int prev = rollingPeakX10000.load(std::memory_order_relaxed);
    while (pk > prev && !rollingPeakX10000.compare_exchange_weak(prev, pk)) {}
    framesDelivered.fetch_add(1, std::memory_order_relaxed);
    samplesDelivered.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
    cb(s, n);
}

void LoopbackCapture::Impl::teardown() {
    // Stop first so no callbacks run, then it is safe to tear down single-thread.
    if (ml) pa_threaded_mainloop_stop(ml);
    if (stream) {
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
        stream = nullptr;
    }
    if (ctx) {
        pa_context_disconnect(ctx);
        pa_context_unref(ctx);
        ctx = nullptr;
    }
    if (ml) {
        pa_threaded_mainloop_free(ml);
        ml = nullptr;
    }
}

// ─── PulseAudio callbacks (mainloop thread, under lock) ──────────────────────

void LoopbackCapture::Impl::contextStateCb(pa_context* c, void* ud) {
    auto* d = static_cast<Impl*>(ud);
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            pa_threaded_mainloop_signal(d->ml, 0);
            break;
        default:
            break;
    }
}

void LoopbackCapture::Impl::streamStateCb(pa_stream* s, void* ud) {
    auto* d = static_cast<Impl*>(ud);
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY:
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            pa_threaded_mainloop_signal(d->ml, 0);
            break;
        default:
            break;
    }
}

void LoopbackCapture::Impl::streamReadCb(pa_stream* s, size_t /*nbytes*/, void* ud) {
    auto* d = static_cast<Impl*>(ud);
    while (pa_stream_readable_size(s) > 0) {
        const void* data = nullptr;
        size_t      len  = 0;
        if (pa_stream_peek(s, &data, &len) < 0) break;
        if (len == 0) break;                       // buffer momentarily empty
        if (data)                                  // data==NULL,len>0 is a hole
            d->deliver(static_cast<const float*>(data),
                       static_cast<int>(len / sizeof(float)));
        pa_stream_drop(s);                         // advance past data or hole
    }
}

// Default-sink name → stashed in monitorName for the start() sequence.
void LoopbackCapture::Impl::serverInfoCb(pa_context*, const pa_server_info* i, void* ud) {
    auto* d = static_cast<Impl*>(ud);
    if (i && i->default_sink_name) d->monitorName = i->default_sink_name;  // sink name for now
    pa_threaded_mainloop_signal(d->ml, 0);
}

// Sink info for the default sink → its monitor source name + native spec.
void LoopbackCapture::Impl::sinkInfoCb(pa_context*, const pa_sink_info* i, int eol, void* ud) {
    auto* d = static_cast<Impl*>(ud);
    if (i && eol == 0) {
        d->monitorName = (i->monitor_source_name && *i->monitor_source_name)
                             ? i->monitor_source_name : "";
        d->nativeRate  = static_cast<int>(i->sample_spec.rate);
        d->nativeCh    = static_cast<int>(i->sample_spec.channels);
    }
    if (eol != 0) pa_threaded_mainloop_signal(d->ml, 0);
}

// ─── public surface ──────────────────────────────────────────────────────────

LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}
LoopbackCapture::~LoopbackCapture() { stop(); }

bool LoopbackCapture::isSupported() { return true; }

std::vector<AudioProcess> LoopbackCapture::enumerateProcesses() {
    std::vector<AudioProcess> out;

    pa_threaded_mainloop* ml = pa_threaded_mainloop_new();
    if (!ml) return out;
    pa_context* ctx = pa_context_new(pa_threaded_mainloop_get_api(ml),
                                     "broaudio-loopback-enum");
    if (!ctx) { pa_threaded_mainloop_free(ml); return out; }

    // Minimal Impl shim just so the shared state callback can signal `ml`.
    struct EnumState { pa_threaded_mainloop* ml; } st{ml};
    auto stateCb = [](pa_context* c, void* ud) {
        auto* s = static_cast<EnumState*>(ud);
        auto x = pa_context_get_state(c);
        if (x == PA_CONTEXT_READY || x == PA_CONTEXT_FAILED || x == PA_CONTEXT_TERMINATED)
            pa_threaded_mainloop_signal(s->ml, 0);
    };
    pa_context_set_state_callback(ctx, stateCb, &st);

    pa_threaded_mainloop_lock(ml);
    bool ok = pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0 &&
              pa_threaded_mainloop_start(ml) >= 0;
    if (ok) {
        for (;;) {
            auto s = pa_context_get_state(ctx);
            if (s == PA_CONTEXT_READY) break;
            if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) { ok = false; break; }
            pa_threaded_mainloop_wait(ml);
        }
    }

    if (ok) {
        struct Collect { std::vector<AudioProcess>* out; pa_threaded_mainloop* ml; }
            col{&out, ml};
        auto cb = [](pa_context*, const pa_sink_input_info* i, int eol, void* ud) {
            auto* c = static_cast<Collect*>(ud);
            if (i && eol == 0) {
                std::uint32_t pid = 0;
                if (propU32(i->proplist, "application.process.id", pid) && pid != 0) {
                    bool seen = false;
                    for (const auto& p : *c->out) if (p.pid == pid) { seen = true; break; }
                    if (!seen) {
                        AudioProcess ap;
                        ap.pid = pid;
                        const char* nm = pa_proplist_gets(i->proplist, "application.process.binary");
                        if (!nm || !*nm) nm = pa_proplist_gets(i->proplist, "application.name");
                        if (!nm || !*nm) nm = i->name;
                        ap.name = (nm && *nm) ? nm : "";
                        c->out->push_back(std::move(ap));
                    }
                }
            }
            if (eol != 0) pa_threaded_mainloop_signal(c->ml, 0);
        };
        waitOp(ml, pa_context_get_sink_input_info_list(ctx, cb, &col));
    }
    pa_threaded_mainloop_unlock(ml);

    pa_threaded_mainloop_stop(ml);
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_threaded_mainloop_free(ml);
    return out;
}

bool LoopbackCapture::start(const LoopbackConfig& cfg, LoopbackCallback cb) {
    if (!cb) return false;
    if (impl_->running.load(std::memory_order_acquire)) return false;

    // PulseAudio cannot capture a single process tree without rerouting it; the
    // process modes are unsupported here (mirrors Windows without the SDK).
    if (cfg.mode != LoopbackMode::SystemOutput) {
        log(LogLevel::Warn, "broaudio loopback: process-scoped capture is not "
            "supported on the PulseAudio backend; use SystemOutput");
        return false;
    }

    impl_->cfg = cfg;
    impl_->cb  = std::move(cb);
    impl_->framesDelivered.store(0, std::memory_order_relaxed);
    impl_->samplesDelivered.store(0, std::memory_order_relaxed);
    impl_->rollingPeakX10000.store(0, std::memory_order_relaxed);
    impl_->monitorName.clear();
    impl_->nativeRate = 0;
    impl_->nativeCh   = 0;

    auto fail = [&](const char* why) {
        log(LogLevel::Error, "broaudio loopback: %s", why);
        impl_->teardown();
        return false;
    };

    impl_->ml = pa_threaded_mainloop_new();
    if (!impl_->ml) return fail("threaded mainloop create failed");
    impl_->ctx = pa_context_new(pa_threaded_mainloop_get_api(impl_->ml),
                                "broaudio-loopback");
    if (!impl_->ctx) return fail("context create failed");
    pa_context_set_state_callback(impl_->ctx, Impl::contextStateCb, impl_.get());

    pa_threaded_mainloop_lock(impl_->ml);

    if (pa_context_connect(impl_->ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0 ||
        pa_threaded_mainloop_start(impl_->ml) < 0) {
        pa_threaded_mainloop_unlock(impl_->ml);
        return fail("server connect failed");
    }

    // Wait for the context to come up (or fail).
    for (;;) {
        auto s = pa_context_get_state(impl_->ctx);
        if (s == PA_CONTEXT_READY) break;
        if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) {
            pa_threaded_mainloop_unlock(impl_->ml);
            return fail("no PulseAudio server");
        }
        pa_threaded_mainloop_wait(impl_->ml);
    }

    // Resolve the default sink, then its monitor source + native spec.
    waitOp(impl_->ml, pa_context_get_server_info(impl_->ctx, Impl::serverInfoCb, impl_.get()));
    std::string sinkName = impl_->monitorName;   // serverInfoCb stashed the sink name
    impl_->monitorName.clear();
    if (sinkName.empty()) {
        pa_threaded_mainloop_unlock(impl_->ml);
        return fail("no default sink");
    }
    waitOp(impl_->ml, pa_context_get_sink_info_by_name(impl_->ctx, sinkName.c_str(),
                                                       Impl::sinkInfoCb, impl_.get()));
    if (impl_->monitorName.empty()) {
        pa_threaded_mainloop_unlock(impl_->ml);
        return fail("default sink has no monitor source");
    }

    // Ask the server for the geometry the listen stack wants; it downmixes +
    // resamples from the monitor for us.
    const int outCh   = cfg.mono ? 1 : (impl_->nativeCh > 0 ? impl_->nativeCh : 2);
    const int outRate = (cfg.targetRate > 0) ? cfg.targetRate
                                             : (impl_->nativeRate > 0 ? impl_->nativeRate : 48000);

    pa_sample_spec spec{};
    spec.format   = PA_SAMPLE_FLOAT32NE;
    spec.rate     = static_cast<std::uint32_t>(outRate);
    spec.channels = static_cast<std::uint8_t>(outCh);
    if (!pa_sample_spec_valid(&spec)) {
        pa_threaded_mainloop_unlock(impl_->ml);
        return fail("invalid sample spec");
    }

    impl_->stream = pa_stream_new(impl_->ctx, "broaudio loopback", &spec, nullptr);
    if (!impl_->stream) {
        pa_threaded_mainloop_unlock(impl_->ml);
        return fail("record stream create failed");
    }
    pa_stream_set_state_callback(impl_->stream, Impl::streamStateCb, impl_.get());
    pa_stream_set_read_callback(impl_->stream, Impl::streamReadCb, impl_.get());

    // ADJUST_LATENCY lets the server pick a fragment size for our spec.
    if (pa_stream_connect_record(impl_->stream, impl_->monitorName.c_str(), nullptr,
                                 PA_STREAM_ADJUST_LATENCY) < 0) {
        pa_threaded_mainloop_unlock(impl_->ml);
        return fail("connect_record failed");
    }

    for (;;) {
        auto s = pa_stream_get_state(impl_->stream);
        if (s == PA_STREAM_READY) break;
        if (s == PA_STREAM_FAILED || s == PA_STREAM_TERMINATED) {
            pa_threaded_mainloop_unlock(impl_->ml);
            return fail("monitor stream did not start");
        }
        pa_threaded_mainloop_wait(impl_->ml);
    }

    impl_->deliveredRate.store(outRate, std::memory_order_relaxed);
    impl_->deliveredChannels.store(outCh, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);

    pa_threaded_mainloop_unlock(impl_->ml);
    return true;
}

void LoopbackCapture::stop() {
    impl_->running.store(false, std::memory_order_release);
    impl_->teardown();
    impl_->deliveredRate.store(0, std::memory_order_relaxed);
    impl_->deliveredChannels.store(0, std::memory_order_relaxed);
}

bool LoopbackCapture::isCapturing() const {
    return impl_->running.load(std::memory_order_acquire);
}

int LoopbackCapture::sampleRate() const {
    return impl_->deliveredRate.load(std::memory_order_relaxed);
}

int LoopbackCapture::channels() const {
    return impl_->deliveredChannels.load(std::memory_order_relaxed);
}

LoopbackStats LoopbackCapture::stats() const {
    LoopbackStats s;
    s.running          = impl_->running.load(std::memory_order_acquire);
    s.framesDelivered  = impl_->framesDelivered.load(std::memory_order_relaxed);
    s.samplesDelivered = impl_->samplesDelivered.load(std::memory_order_relaxed);
    s.rollingPeak      = impl_->rollingPeakX10000.load(std::memory_order_relaxed) / 10000.0f;
    return s;
}

} // namespace broaudio
