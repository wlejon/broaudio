// OfflineAudioContext: a context that renders its node graph as fast as it
// can into an AudioBuffer instead of playing it (Web Audio, "The
// OfflineAudioContext Interface"). Its nodes are the same classes as an
// AudioContext's, made under an OfflineNodeScope: they take nothing from the
// engine and are rendered by host_audio_offline_graph.cpp in 128-frame
// quanta. Rendering starts after the script that called startRendering()
// (a promise job), stops at each suspend(time) until resume(), and ends with
// the promise resolved with the buffer, then `complete`
// (OfflineAudioCompletionEvent) and `statechange`.
//
// Also the nodes only an offline context makes: ConstantSourceNode and
// IIRFilterNode (the live engine has nothing to drive with them).

#include "host_audio_offline.h"

#include <cmath>
#include <complex>
#include <functional>

namespace broaudio::api {

HostClass g_offlineAudioContextClass;
HostClass g_constantSourceNodeClass;
HostClass g_iirFilterNodeClass;
HostClass g_offlineCompletionEventClass;

namespace {

inline constexpr uint32_t kHostOfflineContextTag = 0x4F414358u;  // 'OACX'

struct HostOfflineContext {
    uint32_t tag = kHostOfflineContextTag;
    std::string state = "suspended";
    bool started = false;
    OfflineRender render;
    std::vector<int64_t> suspendFrames;  // pending suspend(time)s, by frame
};

struct HostCompletionEvent {
    uint32_t tag = 0x4F414345u;  // 'OACE'
};

thread_local std::shared_ptr<const OfflineClock> t_creationClock;

HostOfflineContext* offlineContextOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostOfflineContext*>(g_offlineAudioContextClass.unwrap(v));
    return p && p->tag == kHostOfflineContextTag ? p : nullptr;
}

// Run `fn` as a promise job: after the script running now, before the next
// task.
void queueJob(std::function<void()> fn) {
    ev::Persistent p(ev::createPromise());
    ev::resolvePromise(p.get(), ev::undefined());
    auto shared = std::make_shared<std::function<void()>>(std::move(fn));
    ev::Persistent cb(ev::makeFunction([shared](Value, std::span<const Value>) -> Value {
        (*shared)();
        return ev::undefined();
    }, 1));
    ev::Persistent then(ev::getProperty(p.get(), "then"));
    const Value arg = cb.get();
    ev::call(then.get(), p.get(), std::span<const Value>(&arg, 1));
}

void setState(const ev::Persistent& ctx, HostOfflineContext* c, const char* state) {
    if (c->state == state) return;
    c->state = state;
    ev::Persistent target(ctx.get());
    queueJob([target]() { dispatchTargetEvent(target.get(), "statechange", ev::undefined()); });
}

// The pending promise of suspend() at `frame`, kept on the context object as
// `_suspends["f<frame>"]` (traced, like `_targets`).
std::string suspendKey(int64_t frame) { return "f" + std::to_string(frame); }

void renderStep(const ev::Persistent& ctx);

void scheduleRender(const ev::Persistent& ctx) {
    ev::Persistent target(ctx.get());
    queueJob([target]() { renderStep(target); });
}

void renderStep(const ev::Persistent& ctx) {
    HostOfflineContext* c = offlineContextOf(ctx.get());
    if (!c || c->state != "running") return;
    OfflineRender& r = c->render;
    int64_t end = r.length;
    for (int64_t f : c->suspendFrames)
        if (f >= r.frame && f < end) end = f;
    renderOfflineGraph(ctx.get(), r, end);
    c = offlineContextOf(ctx.get());
    if (!c) return;
    if (r.frame < r.length) {
        // At a suspend(time): suspended until resume(), its promise resolved.
        const int64_t frame = r.frame;
        c->suspendFrames.erase(std::remove(c->suspendFrames.begin(), c->suspendFrames.end(), frame),
                               c->suspendFrames.end());
        setState(ctx, c, "suspended");
        ev::Persistent map(ev::getProperty(ctx.get(), "_suspends"));
        if (ev::isObject(map.get())) {
            ev::Persistent promise(ev::getProperty(map.get(), suspendKey(frame)));
            ev::deleteProperty(map.get(), suspendKey(frame));
            if (ev::isObject(promise.get())) ev::resolvePromise(promise.get(), ev::undefined());
        }
        return;
    }
    // Done: the buffer, the promise, then `complete`.
    ev::Persistent buffer(makeAudioBufferValue(r.channels, static_cast<int>(r.length), static_cast<int>(r.sampleRate)));
    if (HostAudioBuffer* hb = hostAudioBufferOf(buffer.get())) hb->channels = std::move(r.output);
    r.output.clear();
    c = offlineContextOf(ctx.get());
    if (c) setState(ctx, c, "closed");
    ev::Persistent promise(ev::getProperty(ctx.get(), "_renderPromise"));
    if (ev::isObject(promise.get())) ev::resolvePromise(promise.get(), buffer.get());
    ev::Persistent target(ctx.get());
    queueJob([target, buffer]() {
        ev::Persistent evt(g_offlineCompletionEventClass.make(new HostCompletionEvent(), [](void* p) {
            delete static_cast<HostCompletionEvent*>(p);
        }));
        {
            ev::Persistent typeStr(ev::fromUtf8("complete"));
            ev::setProperty(evt.get(), "type", typeStr.get());
        }
        ev::setProperty(evt.get(), "renderedBuffer", buffer.get());
        ev::setProperty(evt.get(), "target", target.get());
        ev::setProperty(evt.get(), "currentTarget", target.get());
        dispatchTargetEvent(target.get(), "complete", evt.get());
    });
}

Value rejected(const char* name, const std::string& message) {
    ev::Persistent p(ev::createPromise());
    ev::Persistent err(hostMakeDomError(name, message));
    ev::rejectPromise(p.get(), err.get());
    return p.get();
}

// The offline context's AudioListener: its params on the context's clock.
Value makeOfflineListener() {
    ObjectBuilder b;
    const struct { const char* name; float value; } params[] = {
        {"positionX", 0}, {"positionY", 0}, {"positionZ", 0}, {"forwardX", 0}, {"forwardY", 0},
        {"forwardZ", -1}, {"upX", 0}, {"upY", 1}, {"upZ", 0}};
    for (const auto& p : params)
        b.set(p.name, makeAudioParamValue(AudioParamTarget::Generic, -1, p.value, -3.4e38f, 3.4e38f, p.value));
    auto setter = [](std::vector<const char*> names) {
        return [names](Value self_, std::span<const Value> a) -> Value {
            ev::Persistent self(self_);
            for (size_t i = 0; i < names.size(); ++i) {
                HostAudioParam* p = hostAudioParamOf(ev::getProperty(self.get(), names[i]));
                if (p) syncAudioParamValue(p, static_cast<float>(numAt(a, i)));
            }
            return ev::undefined();
        };
    };
    b.def("setPosition", 3, setter({"positionX", "positionY", "positionZ"}));
    b.def("setOrientation", 6, setter({"forwardX", "forwardY", "forwardZ", "upX", "upY", "upZ"}));
    return b.get();
}

// The channel mixing Web Audio gives each kind of node.
void setChannelDefaults(HostAudioNode& n) {
    switch (n.nodeType) {
    case AudioNodeType::StereoPanner:
    case AudioNodeType::Panner:
    case AudioNodeType::Convolver:
    case AudioNodeType::DynamicsCompressor:
        n.channelCount = 2;
        n.channelCountMode = HostAudioNode::kModeClampedMax;
        break;
    case AudioNodeType::ChannelSplitter:
        n.channelCount = static_cast<uint32_t>(reinterpret_cast<HostChannelSplitterNode&>(n).numberOfOutputs);
        n.channelCountMode = HostAudioNode::kModeExplicit;
        n.discrete = true;
        break;
    case AudioNodeType::ChannelMerger:
        n.channelCount = 1;
        n.channelCountMode = HostAudioNode::kModeExplicit;
        break;
    default:
        break;
    }
}

// ---- ConstantSourceNode / IIRFilterNode ------------------------------------------

HostConstantSourceNode* constantSourceOf(Value v) {
    return nodeOfKind<HostConstantSourceNode>(v, AudioNodeType::ConstantSource);
}

Value makeConstantSourceNodeValue() {
    auto* n = new HostConstantSourceNode();
    n->base.nodeType = AudioNodeType::ConstantSource;
    n->offsetParam = makeAudioParam(AudioParamTarget::Generic, -1, 1.0f, -3.4e38f, 3.4e38f, 1.0f);
    ObjectBuilder b(g_constantSourceNodeClass.make(n, [](void* p) { delete static_cast<HostConstantSourceNode*>(p); }));
    b.set("offset", makeAudioParamValue(n->offsetParam));
    b.set("onended", ev::null());
    return b.get();
}

void decorateConstantSourceProto(ObjectBuilder& b) {
    b.def("start", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostConstantSourceNode* n = constantSourceOf(self_);
        if (!n) return ev::undefined();
        if (n->started) return throwInvalidStateError("ConstantSourceNode cannot be started more than once");
        const double when = hasArg(a, 0) ? numAt(a, 0) : 0.0;
        if (!(when >= 0.0)) return ev::throwRangeError("ConstantSourceNode.start: when must be a non-negative number");
        n->started = true;
        n->startTime = std::max(when, n->offsetParam->now());
        return ev::undefined();
    });
    b.def("stop", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostConstantSourceNode* n = constantSourceOf(self_);
        if (!n) return ev::undefined();
        if (!n->started) return throwInvalidStateError("ConstantSourceNode.stop: the node has not been started");
        const double when = hasArg(a, 0) ? numAt(a, 0) : 0.0;
        if (!(when >= 0.0)) return ev::throwRangeError("ConstantSourceNode.stop: when must be a non-negative number");
        n->stopTime = when;
        return ev::undefined();
    });
}

bool readCoefficients(Value v, const char* what, std::vector<double>& out) {
    std::vector<float> f;
    if (!readFloatArrayArg(v, FloatArrayArg::Float32OrPlain, what, f)) return false;
    out.assign(f.begin(), f.end());
    return true;
}

Value makeIIRFilterNodeValue(std::vector<double> ff, std::vector<double> fb) {
    auto* n = new HostIIRFilterNode();
    n->base.nodeType = AudioNodeType::IIRFilter;
    n->feedforward = std::move(ff);
    n->feedback = std::move(fb);
    return g_iirFilterNodeClass.make(n, [](void* p) { delete static_cast<HostIIRFilterNode*>(p); });
}

void decorateIIRFilterProto(ObjectBuilder& b) {
    // getFrequencyResponse(frequencyHz, magResponse, phaseResponse).
    b.def("getFrequencyResponse", 3, [](Value self_, std::span<const Value> a) -> Value {
        auto* n = nodeOfKind<HostIIRFilterNode>(self_, AudioNodeType::IIRFilter);
        if (!n || a.size() < 3) return ev::undefined();
        std::vector<float> freqs;
        if (!readFloatArrayArg(a[0], FloatArrayArg::Float32Only, "getFrequencyResponse: frequencyHz", freqs))
            return ev::undefined();
        ev::TypedArrayInfo mag = ev::typedArrayInfo(a[1]);
        ev::TypedArrayInfo ph = ev::typedArrayInfo(a[2]);
        if (!mag || !ph || mag.elementCount < freqs.size() || ph.elementCount < freqs.size())
            return throwDomException("InvalidAccessError", "getFrequencyResponse: arrays of unequal length");
        const double sr = contextSampleRate(ev::getProperty(self_, "context"));
        mag = ev::typedArrayInfo(a[1]);
        ph = ev::typedArrayInfo(a[2]);
        auto* m = reinterpret_cast<float*>(mag.data);
        auto* p = reinterpret_cast<float*>(ph.data);
        for (size_t i = 0; i < freqs.size(); ++i) {
            const double w = 2.0 * M_PI * freqs[i] / sr;
            std::complex<double> num, den;
            for (size_t k = 0; k < n->feedforward.size(); ++k) num += n->feedforward[k] * std::polar(1.0, -w * k);
            for (size_t k = 0; k < n->feedback.size(); ++k) den += n->feedback[k] * std::polar(1.0, -w * k);
            const std::complex<double> h = num / den;
            const bool valid = freqs[i] >= 0.0f && freqs[i] <= sr / 2.0;
            m[i] = valid ? static_cast<float>(std::abs(h)) : NAN;
            p[i] = valid ? static_cast<float>(std::arg(h)) : NAN;
        }
        return ev::undefined();
    });
}

// ---- OfflineAudioContext ----------------------------------------------------------

Value constructOfflineContext(std::span<const Value> a) {
    // (numberOfChannels, length, sampleRate) or ({numberOfChannels, length, sampleRate}).
    double channels = 1, length = 0, rate = 0;
    if (!a.empty() && ev::isObject(a[0])) {
        ev::Persistent opts(a[0]);
        Value v = ev::getProperty(opts.get(), "numberOfChannels");
        if (!ev::isUndefined(v)) channels = ev::toDouble(v);
        v = ev::getProperty(opts.get(), "length");
        if (ev::isUndefined(v)) return ev::throwTypeError("OfflineAudioContext: options.length is required");
        length = ev::toDouble(v);
        v = ev::getProperty(opts.get(), "sampleRate");
        if (ev::isUndefined(v)) return ev::throwTypeError("OfflineAudioContext: options.sampleRate is required");
        rate = ev::toDouble(v);
    } else {
        if (a.size() < 3) return ev::throwTypeError("OfflineAudioContext: 3 arguments required");
        channels = numAt(a, 0);
        length = numAt(a, 1);
        rate = numAt(a, 2);
    }
    if (!(channels >= 1 && channels <= 32))
        return throwNotSupportedError("OfflineAudioContext: numberOfChannels must be in 1..32");
    if (!(length >= 1 && length <= (1 << 28) / channels))
        return throwNotSupportedError("OfflineAudioContext: length must be positive");
    if (!(rate >= 3000 && rate <= 768000))
        return throwNotSupportedError("OfflineAudioContext: sampleRate must be in 3000..768000");

    auto* c = new HostOfflineContext();
    c->render.channels = static_cast<int>(channels);
    c->render.length = static_cast<int64_t>(length);
    c->render.sampleRate = std::floor(rate);
    c->render.clock = std::make_shared<OfflineClock>();
    ev::Persistent ctx(g_offlineAudioContextClass.make(c, [](void* p) { delete static_cast<HostOfflineContext*>(p); }));
    {
        ev::Persistent nodes(ev::makeArray(0));
        ev::setProperty(ctx.get(), "_nodes", nodes.get());
    }
    {
        ev::Persistent suspends(ev::createObject());
        ev::setProperty(ctx.get(), "_suspends", suspends.get());
    }
    {
        OfflineNodeScope scope(ctx.get());
        ev::Persistent dest(makeDestinationNodeValue());
        if (HostAudioNode* d = hostAudioNodeOf(dest.get())) {
            d->offline = true;
            d->channelCount = static_cast<uint32_t>(channels);
            d->channelCountMode = HostAudioNode::kModeExplicit;
        }
        ev::setProperty(dest.get(), "context", ctx.get());
        ev::setProperty(ctx.get(), "destination", dest.get());
        ev::Persistent listener(makeOfflineListener());
        ev::setProperty(ctx.get(), "listener", listener.get());
    }
    ev::setProperty(ctx.get(), "oncomplete", ev::null());
    ev::setProperty(ctx.get(), "onstatechange", ev::null());
    return ctx.get();
}

void decorateOfflineContextProto(ObjectBuilder& b) {
    b.accessor("length", [](Value self_, std::span<const Value>) {
        HostOfflineContext* c = offlineContextOf(self_);
        return ev::fromDouble(c ? static_cast<double>(c->render.length) : 0.0);
    }, nullptr);
    b.accessor("sampleRate", [](Value self_, std::span<const Value>) {
        HostOfflineContext* c = offlineContextOf(self_);
        return ev::fromDouble(c ? c->render.sampleRate : 0.0);
    }, nullptr);
    b.accessor("currentTime", [](Value self_, std::span<const Value>) {
        HostOfflineContext* c = offlineContextOf(self_);
        return ev::fromDouble(c ? c->render.clock->time : 0.0);
    }, nullptr);
    b.accessor("state", [](Value self_, std::span<const Value>) {
        HostOfflineContext* c = offlineContextOf(self_);
        return ev::fromUtf8(c ? c->state : "closed");
    }, nullptr);

    decorateBaseAudioContextMembers(b);
    decorateEventTargetMembers(b);

    b.def("createConstantSource", 0, [](Value self_, std::span<const Value>) {
        OfflineNodeScope scope(self_);
        return scope.finish(makeConstantSourceNodeValue());
    });
    b.def("createIIRFilter", 2, [](Value self_, std::span<const Value> a) -> Value {
        std::vector<double> ff, fb;
        if (a.size() < 2 || !readCoefficients(a[0], "createIIRFilter: feedforward", ff) ||
            !readCoefficients(a[1], "createIIRFilter: feedback", fb))
            return ev::isUndefined(a.empty() ? ev::undefined() : a[0]) ? ev::throwTypeError("createIIRFilter: 2 arguments required")
                                                                      : ev::undefined();
        if (ff.empty() || ff.size() > 20 || fb.empty() || fb.size() > 20)
            return throwNotSupportedError("createIIRFilter: 1 to 20 coefficients each");
        if (fb[0] == 0.0) return throwDomException("InvalidStateError", "createIIRFilter: feedback[0] is 0");
        bool nonzero = false;
        for (double v : ff) nonzero = nonzero || v != 0.0;
        if (!nonzero) return throwDomException("InvalidStateError", "createIIRFilter: feedforward is all zeros");
        OfflineNodeScope scope(self_);
        return scope.finish(makeIIRFilterNodeValue(std::move(ff), std::move(fb)));
    });

    b.def("startRendering", 0, [](Value self_, std::span<const Value>) -> Value {
        ev::Persistent ctx(self_);
        HostOfflineContext* c = offlineContextOf(ctx.get());
        if (!c) return ev::throwTypeError("startRendering: not an OfflineAudioContext");
        if (c->started) return rejected("InvalidStateError", "startRendering: already started");
        c->started = true;
        c->render.output.assign(static_cast<size_t>(c->render.channels),
                                std::vector<float>(static_cast<size_t>(c->render.length), 0.0f));
        ev::Persistent promise(ev::createPromise());
        ev::setProperty(ctx.get(), "_renderPromise", promise.get());
        c = offlineContextOf(ctx.get());
        setState(ctx, c, "running");
        scheduleRender(ctx);
        return promise.get();
    });

    // suspend(suspendTime): rendering stops at that time (rounded up to a
    // render quantum) and the promise resolves; resume() goes on.
    b.def("suspend", 1, [](Value self_, std::span<const Value> a) -> Value {
        ev::Persistent ctx(self_);
        HostOfflineContext* c = offlineContextOf(ctx.get());
        if (!c) return ev::throwTypeError("suspend: not an OfflineAudioContext");
        const double t = numAt(a, 0);
        if (!(t >= 0.0)) return rejected("InvalidStateError", "suspend: negative suspend time");
        int64_t frame = static_cast<int64_t>(std::ceil(t * c->render.sampleRate / kRenderQuantum)) * kRenderQuantum;
        if (frame >= c->render.length || frame < c->render.frame || (c->started && frame == c->render.frame))
            return rejected("InvalidStateError", "suspend: the time is outside what is left to render");
        for (int64_t f : c->suspendFrames)
            if (f == frame) return rejected("InvalidStateError", "suspend: a suspend is already scheduled at that time");
        c->suspendFrames.push_back(frame);
        ev::Persistent promise(ev::createPromise());
        ev::Persistent map(ev::getProperty(ctx.get(), "_suspends"));
        if (ev::isObject(map.get())) ev::setProperty(map.get(), suspendKey(frame), promise.get());
        return promise.get();
    });

    b.def("resume", 0, [](Value self_, std::span<const Value>) -> Value {
        ev::Persistent ctx(self_);
        HostOfflineContext* c = offlineContextOf(ctx.get());
        if (!c) return ev::throwTypeError("resume: not an OfflineAudioContext");
        if (!c->started) return rejected("InvalidStateError", "resume: rendering has not started");
        if (c->state == "closed") return rejected("InvalidStateError", "resume: the context is closed");
        ev::Persistent promise(ev::createPromise());
        if (c->state == "suspended") {
            setState(ctx, c, "running");
            scheduleRender(ctx);
        }
        ev::resolvePromise(promise.get(), ev::undefined());
        return promise.get();
    });
}

}  // namespace

std::shared_ptr<const OfflineClock> offlineCreationClock() { return t_creationClock; }

int contextSampleRate(Value context) {
    if (HostOfflineContext* c = offlineContextOf(context)) return static_cast<int>(c->render.sampleRate);
    auto* e = getAudioEngine();
    return e ? e->sampleRate() : 44100;
}

OfflineNodeScope::OfflineNodeScope(Value context) {
    HostOfflineContext* c = offlineContextOf(context);
    if (!c) return;
    active_ = true;
    context_.set(context);
    previous_ = t_creationClock;
    t_creationClock = c->render.clock;
}

OfflineNodeScope::~OfflineNodeScope() {
    if (active_) t_creationClock = previous_;
}

Value OfflineNodeScope::finish(Value node) {
    if (!active_) return node;
    ev::Persistent n(node);
    HostAudioNode* h = hostAudioNodeOf(n.get());
    if (!h) return n.get();
    h->offline = true;
    setChannelDefaults(*h);
    ev::setProperty(n.get(), "context", context_.get());
    ev::Persistent list(ev::getProperty(context_.get(), "_nodes"));
    if (ev::isObject(list.get())) {
        Value lenV = ev::getProperty(list.get(), "length");
        const uint32_t len = ev::isNumber(lenV) ? saturateU32(ev::toDouble(lenV)) : 0;
        ev::setElement(list.get(), len, n.get());
    }
    return n.get();
}

void installOfflineAudioGlobals() {
    g_offlineAudioContextClass.install(
        "OfflineAudioContext", 3, [](Value, std::span<const Value> a) { return constructOfflineContext(a); },
        decorateOfflineContextProto);
    g_constantSourceNodeClass.install(
        "ConstantSourceNode", 1,
        [](Value, std::span<const Value> a) -> Value {
            if (a.empty() || !offlineContextOf(a[0]))
                return throwNotSupportedError("ConstantSourceNode: needs an OfflineAudioContext");
            OfflineNodeScope scope(a[0]);
            return scope.finish(makeConstantSourceNodeValue());
        },
        decorateConstantSourceProto);
    g_constantSourceNodeClass.inherit(g_audioNodeClass);
    g_iirFilterNodeClass.install(
        "IIRFilterNode", 2,
        [](Value, std::span<const Value> a) -> Value {
            return throwNotSupportedError("IIRFilterNode: use OfflineAudioContext.createIIRFilter");
        },
        decorateIIRFilterProto);
    g_iirFilterNodeClass.inherit(g_audioNodeClass);
    g_offlineCompletionEventClass.install(
        "OfflineAudioCompletionEvent", 2,
        [](Value, std::span<const Value> a) -> Value {
            ev::Persistent type(a.empty() ? ev::fromUtf8("") : a[0]);
            ev::Persistent buffer(a.size() >= 2 && ev::isObject(a[1]) ? ev::getProperty(a[1], "renderedBuffer") : ev::undefined());
            if (!hostAudioBufferOf(buffer.get()))
                return ev::throwTypeError("OfflineAudioCompletionEvent: renderedBuffer must be an AudioBuffer");
            ev::Persistent evt(g_offlineCompletionEventClass.make(new HostCompletionEvent(), [](void* p) {
                delete static_cast<HostCompletionEvent*>(p);
            }));
            ev::Persistent typeStr(ev::fromUtf8(ev::toUtf8(type.get())));
            ev::setProperty(evt.get(), "type", typeStr.get());
            ev::setProperty(evt.get(), "renderedBuffer", buffer.get());
            return evt.get();
        },
        nullptr);
}

}  // namespace broaudio::api
