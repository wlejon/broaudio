// `SynthGraph`: a synthesis graph described by a plain object
// (broaudio/synth/synth_graph.h; the format is documented in bro's
// docs/audio-synth-graph-api.js), rendered offline to an AudioBuffer or
// played live through the engine.
//
//   const g = new SynthGraph(desc)          // or ctx.createSynthGraph(desc)
//   g.params / g.paramNames / g.layers / g.compiled / g.precompile()
//   g.render({sampleRate, seed, params, jitter, maxDuration, compiled, loop})
//   ctx.playSynth(g, {seed, params, jitter, gain, when, pan, bus, position, loop})
//     loop: {length, crossfade, start, snap, curve, releaseFade}
//   ctx.releaseSynth(id)
//
// The description crosses as JSON.stringify of the object (a string is taken
// as JSON already). Every validation failure is a TypeError, or a RangeError
// for a well-formed number out of range, whose message starts with
// "SynthGraph: " and the path of the offending field.

#include "host_audio_synth_graph.h"
#include "host_audio_internal.h"
#include "api.h"

#include <broaudio/synth/synth_graph.h>

#include <initializer_list>

namespace broaudio::api {

namespace {

HostClass g_synthGraphClass;

struct HostSynthGraph {
    std::shared_ptr<const SynthGraph> graph;
    // The seed of the next playSynth that names none: every trigger differs.
    std::atomic<uint32_t> nextSeed{1};
};

void destroySynthGraph(void* p) { delete static_cast<HostSynthGraph*>(p); }

HostSynthGraph* synthGraphOf(Value v) { return static_cast<HostSynthGraph*>(g_synthGraphClass.unwrap(v)); }

[[noreturn]] void throwSynth(const std::string& msg, bool range) {
    const std::string m = "SynthGraph: " + msg;
    if (range) ev::throwRangeError(m);
    ev::throwTypeError(m);
}

// `ns.fn(arg)` for a global namespace object (JSON.stringify, Object.keys).
// Throws what the call throws.
Value callGlobal(const char* ns, const char* fn, Value arg) {
    ev::Persistent a(arg);
    ev::GlobalValue g = ev::globalValue(ns);
    if (!g.found || !ev::isObject(g.value)) throwSynth(std::string(ns) + " is missing from this realm", false);
    ev::Persistent obj(g.value);
    ev::Persistent f(ev::getProperty(obj.get(), fn));
    const Value args[1] = {a.get()};
    ev::CallResult r = ev::call(f.get(), obj.get(), std::span<const Value>(args, 1));
    if (r.thrown) ev::throwValue(r.value);
    return r.value;
}

// The JSON text of a description or params argument.
std::string jsonOf(Value v, const char* what) {
    if (ev::isString(v)) return ev::toUtf8(v);
    if (!ev::isObject(v) || ev::isFunction(v)) throwSynth(std::string(what) + " must be an object", false);
    Value s = callGlobal("JSON", "stringify", v);
    if (!ev::isString(s)) throwSynth(std::string(what) + " must be an object", false);
    return ev::toUtf8(s);
}

// Every own key of `opts` is one of `allowed`, else a TypeError naming it.
void checkKeys(Value opts, std::initializer_list<std::string_view> allowed, const std::string& who) {
    ev::Persistent keys(callGlobal("Object", "keys", opts));
    const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(keys.get(), "length")));
    for (uint32_t i = 0; i < n; ++i) {
        const std::string k = ev::toUtf8(ev::getElement(keys.get(), i));
        bool ok = false;
        for (std::string_view a : allowed) ok = ok || a == k;
        if (!ok) {
            std::string list;
            for (std::string_view a : allowed) list += (list.empty() ? "" : ", ") + std::string(a);
            throwSynth(who + ": unknown option '" + k + "' (expected " + list + ")", false);
        }
    }
}

// Option readers over a rooted options object. A missing (undefined) option
// leaves `out` alone; a present one of the wrong type throws.
struct Opts {
    ev::Persistent obj;
    std::string who;

    bool has(std::string_view name) const {
        return ev::isObject(obj.get()) && !ev::isUndefined(ev::getProperty(obj.get(), name));
    }
    bool number(std::string_view name, double& out, double lo, double hi) const {
        if (!has(name)) return false;
        Value v = ev::getProperty(obj.get(), name);
        if (!ev::isNumber(v)) throwSynth(who + ": " + std::string(name) + " must be a number", false);
        const double d = ev::toDouble(v);
        if (!(d >= lo && d <= hi)) {
            throwSynth(who + ": " + std::string(name) + " is " + std::to_string(d) + ", outside [" +
                           std::to_string(lo) + ", " + std::to_string(hi) + "]",
                       true);
        }
        out = d;
        return true;
    }
    bool boolean(std::string_view name, bool& out) const {
        if (!has(name)) return false;
        Value v = ev::getProperty(obj.get(), name);
        if (!ev::isBool(v)) throwSynth(who + ": " + std::string(name) + " must be a boolean", false);
        out = ev::toBool(v);
        return true;
    }
    bool seed(uint32_t& out) const {
        double d = 0;
        if (!number("seed", d, 0.0, 4294967295.0)) return false;
        if (d != static_cast<double>(static_cast<uint32_t>(d))) throwSynth(who + ": seed must be an integer", true);
        out = static_cast<uint32_t>(d);
        return true;
    }
};

// Opts over argument `i` (undefined when absent), keys checked.
Opts readOpts(std::span<const Value> a, size_t i, const std::string& who,
              std::initializer_list<std::string_view> allowed) {
    Opts o{ev::Persistent(i < a.size() ? a[i] : ev::undefined()), who};
    if (ev::isUndefined(o.obj.get()) || ev::isNull(o.obj.get())) {
        o.obj.set(ev::undefined());
        return o;
    }
    if (!ev::isObject(o.obj.get()) || ev::isFunction(o.obj.get())) throwSynth(who + ": options must be an object", false);
    checkKeys(o.obj.get(), allowed, who);
    return o;
}

// The trigger an options object describes: seed, params (overrides), jitter.
void readTrigger(const Opts& o, const SynthGraph& g, SynthTrigger& t) {
    o.seed(t.seed);
    o.boolean("jitter", t.jitter);
    if (!o.has("params")) return;
    const std::string json = jsonOf(ev::getProperty(o.obj.get(), "params"), (o.who + ": params").c_str());
    std::string err;
    bool range = false;
    try {
        t.overrides = g.overridesFromJson(json);
    } catch (const SynthGraphError& e) {
        err = e.what();
        range = e.isRange();
    }
    if (!err.empty()) throwSynth(o.who + ": " + err, range);   // err names "params.<name>"
}

// `loop` of an options object into `out` (throws with the caller's prefix).
void readLoop(const Opts& o, SynthLoopOptions& out) {
    if (!o.has("loop")) return;
    bool range = false;
    const std::string err = readSynthLoopOptions(ev::getProperty(o.obj.get(), "loop"), out, range);
    if (!err.empty()) throwSynth(o.who + ": " + err, range);
}

std::shared_ptr<const SynthGraph> parseGraph(Value desc) {
    const std::string json = jsonOf(desc, "the description");
    std::string err;
    bool range = false;
    std::shared_ptr<const SynthGraph> g;
    try {
        g = SynthGraph::fromJson(json);
    } catch (const SynthGraphError& e) {
        err = e.what();
        range = e.isRange();
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!g) throwSynth(err.empty() ? "invalid description" : err, range);
    return g;
}

Value makeSynthGraphValue(Value desc) {
    auto g = parseGraph(desc);
    auto* h = new HostSynthGraph();
    h->graph = std::move(g);
    return g_synthGraphClass.make(h, destroySynthGraph);
}

HostSynthGraph* thisGraph(Value self, const char* what) {
    HostSynthGraph* h = synthGraphOf(self);
    if (!h) throwSynth(std::string(what) + " called on an object that is not a SynthGraph", false);
    return h;
}

Value renderFn(Value self, std::span<const Value> a) {
    HostSynthGraph* h = thisGraph(self, "render");
    const std::shared_ptr<const SynthGraph> g = h->graph;
    Opts o = readOpts(a, 0, "render", {"sampleRate", "seed", "params", "jitter", "maxDuration", "compiled", "loop"});
    SynthRenderOptions ro;
    Engine* eng = existingAudioEngine();
    ro.sampleRate = eng && eng->sampleRate() > 0 ? eng->sampleRate() : 48000;
    double d = 0;
    if (o.number("sampleRate", d, 8000.0, 384000.0)) ro.sampleRate = static_cast<int>(d);
    if (o.number("maxDuration", d, 0.0, 600.0)) ro.maxSeconds = d;
    o.boolean("compiled", ro.compiled);
    readTrigger(o, *g, ro.trigger);
    readLoop(o, ro.loop);

    const std::vector<float> samples = renderSynth(g, ro);
    ev::Persistent buf(makeAudioBufferValue(1, static_cast<int>(samples.size()), ro.sampleRate));
    HostAudioBuffer* hb = hostAudioBufferOf(buf.get());
    if (hb && !hb->channels.empty()) {
        std::copy(samples.begin(), samples.end(), hb->channels[0].begin());
    }
    return buf.get();
}

Value paramsGetter(Value self, std::span<const Value>) {
    HostSynthGraph* h = thisGraph(self, "params");
    const std::shared_ptr<const SynthGraph> g = h->graph;
    ObjectBuilder out;
    for (const SynthParamInfo& p : g->params()) {
        ObjectBuilder o;
        o.set("value", static_cast<double>(p.value));
        o.set("jitter", static_cast<double>(p.jitter));
        o.set("jitterAbs", static_cast<double>(p.jitterAbs));
        o.set("min", static_cast<double>(p.lo));
        o.set("max", static_cast<double>(p.hi));
        out.set(p.name, o.get());
    }
    return out.get();
}

Value paramNamesGetter(Value self, std::span<const Value>) {
    HostSynthGraph* h = thisGraph(self, "paramNames");
    const std::shared_ptr<const SynthGraph> g = h->graph;
    return hostArrayOf(g->params().size(), [&](size_t i) { return ev::fromUtf8(g->params()[i].name); });
}

Value layersGetter(Value self, std::span<const Value>) {
    HostSynthGraph* h = thisGraph(self, "layers");
    const std::shared_ptr<const SynthGraph> g = h->graph;
    return hostArrayOf(static_cast<size_t>(g->layerCount()), [&](size_t i) {
        ObjectBuilder o;
        o.set("id", g->layerId(static_cast<int>(i)));
        o.set("shape", g->layerShapeKey(static_cast<int>(i)));
        return o.get();
    });
}

void decorateSynthGraphProto(ObjectBuilder& b) {
    b.accessor("params", paramsGetter);
    b.accessor("paramNames", paramNamesGetter);
    b.accessor("layers", layersGetter);
    b.accessor("compiled", [](Value self, std::span<const Value>) {
        return ev::fromBool(thisGraph(self, "compiled")->graph->compiled());
    });
    b.def("precompile", 0, [](Value self, std::span<const Value>) {
        const std::shared_ptr<const SynthGraph> g = thisGraph(self, "precompile")->graph;
        return ev::fromBool(g->precompile());
    });
    b.def("render", 1, renderFn);
}

// The SynthGraph a playSynth argument names: an instance, or a description
// parsed on the spot.
std::shared_ptr<const SynthGraph> graphArg(Value v, HostSynthGraph** host) {
    if (HostSynthGraph* h = synthGraphOf(v)) {
        *host = h;
        return h->graph;
    }
    *host = nullptr;
    return parseGraph(v);
}

std::atomic<uint32_t> g_descSeed{1};   // seeds for playSynth on a bare description

Value playSynthFn(Value, std::span<const Value> a) {
    if (a.empty()) throwSynth("playSynth(graph, opts?): graph is required", false);
    ev::Persistent graphV(a[0]);
    HostSynthGraph* host = nullptr;
    const std::shared_ptr<const SynthGraph> g = graphArg(graphV.get(), &host);
    Opts o = readOpts(a, 1, "playSynth",
                      {"seed", "params", "jitter", "gain", "when", "pan", "bus", "position", "loop"});
    Engine::SynthPlayOptions po;
    if (!o.seed(po.trigger.seed)) {
        po.trigger.seed = host ? host->nextSeed.fetch_add(1, std::memory_order_relaxed)
                               : g_descSeed.fetch_add(1, std::memory_order_relaxed);
    }
    readTrigger(o, *g, po.trigger);
    readLoop(o, po.loop);
    double d = 0;
    if (o.number("gain", d, 0.0, 1000.0)) po.gain = static_cast<float>(d);
    if (o.number("when", d, 0.0, 1e9)) po.when = d;
    if (o.number("pan", d, -1.0, 1.0)) po.pan = static_cast<float>(d);
    if (o.number("bus", d, 0.0, 1e6)) po.busId = static_cast<int>(d);
    if (o.has("position")) {
        ev::Persistent pos(ev::getProperty(o.obj.get(), "position"));
        const bool arrayLike = ev::isObject(pos.get()) && ev::isNumber(ev::getProperty(pos.get(), "length"));
        if (!arrayLike || ev::toDouble(ev::getProperty(pos.get(), "length")) != 3) {
            throwSynth("playSynth: position must be [x, y, z]", false);
        }
        for (uint32_t i = 0; i < 3; ++i) {
            Value e = ev::getElement(pos.get(), i);
            if (!ev::isNumber(e) || !std::isfinite(ev::toDouble(e))) {
                throwSynth("playSynth: position[" + std::to_string(i) + "] must be a finite number", false);
            }
            po.position[i] = static_cast<float>(ev::toDouble(e));
        }
        po.spatial = true;
    }
    Engine* e = getAudioEngine();
    if (!e) return ev::fromDouble(-1);
    return ev::fromDouble(e->playSynth(g, po));
}

} // namespace

std::shared_ptr<const SynthGraph> synthGraphFromValue(Value v) {
    ev::Persistent root(v);
    HostSynthGraph* host = nullptr;
    return graphArg(root.get(), &host);
}

std::string readSynthLoopOptions(Value v, SynthLoopOptions& out, bool& range) {
    range = false;
    ev::Persistent obj(v);
    if (!ev::isObject(obj.get()) || ev::isFunction(obj.get()))
        return "loop must be an object {length, crossfade, start, snap, curve, releaseFade}";
    static const char* const kKeys[] = {"length", "crossfade", "start", "snap", "curve", "releaseFade"};
    {
        ev::Persistent keys(callGlobal("Object", "keys", obj.get()));
        const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(keys.get(), "length")));
        for (uint32_t i = 0; i < n; ++i) {
            const std::string k = ev::toUtf8(ev::getElement(keys.get(), i));
            bool ok = false;
            for (const char* a : kKeys) ok = ok || k == a;
            if (!ok) return "loop: unknown option '" + k + "' (expected length, crossfade, start, snap, curve, releaseFade)";
        }
    }
    SynthLoopOptions o;
    std::string err;
    auto num = [&](const char* name, double& dst, double lo, double hi, bool required) {
        if (!err.empty()) return;
        Value x = ev::getProperty(obj.get(), name);
        if (ev::isUndefined(x)) {
            if (required) err = std::string("loop.") + name + ": required";
            return;
        }
        if (!ev::isNumber(x)) { err = std::string("loop.") + name + " must be a number"; return; }
        const double d = ev::toDouble(x);
        if (!(d >= lo && d <= hi)) {
            err = std::string("loop.") + name + " is " + std::to_string(d) + ", outside [" + std::to_string(lo) +
                  ", " + std::to_string(hi) + "]";
            range = true;
            return;
        }
        dst = d;
    };
    num("length", o.length, 0.001, 60.0, true);
    num("crossfade", o.crossfade, 0.0, 30.0, false);
    num("start", o.start, 0.0, 600.0, false);
    num("releaseFade", o.releaseFade, 0.0, 10.0, false);
    if (!err.empty()) return err;
    Value snap = ev::getProperty(obj.get(), "snap");
    if (!ev::isUndefined(snap)) {
        if (!ev::isBool(snap)) return "loop.snap must be a boolean";
        o.snap = ev::toBool(snap);
    }
    Value curve = ev::getProperty(obj.get(), "curve");
    if (!ev::isUndefined(curve)) {
        const std::string c = ev::isString(curve) ? ev::toUtf8(curve) : std::string();
        if (c == "auto") o.curve = SynthLoopCurve::Auto;
        else if (c == "power") o.curve = SynthLoopCurve::Power;
        else if (c == "linear") o.curve = SynthLoopCurve::Linear;
        else return "loop.curve must be 'auto', 'power' or 'linear'";
    }
    out = o;
    return {};
}

Value synthLoopOptionsValue(const SynthLoopOptions& o) {
    ObjectBuilder b;
    b.set("length", o.length);
    b.set("crossfade", o.crossfade);
    b.set("start", o.start);
    b.set("snap", o.snap);
    b.set("curve", std::string(o.curve == SynthLoopCurve::Power    ? "power"
                               : o.curve == SynthLoopCurve::Linear ? "linear"
                                                                   : "auto"));
    b.set("releaseFade", o.releaseFade);
    return b.get();
}

void installSynthGraphClass() {
    g_synthGraphClass.install(
        "SynthGraph", 1,
        [](Value, std::span<const Value> a) -> Value {
            if (a.empty()) throwSynth("new SynthGraph(description): description is required", false);
            return makeSynthGraphValue(a[0]);
        },
        decorateSynthGraphProto);
}

void registerAudioContextSynthGraph(ObjectBuilder& b) {
    b.def("createSynthGraph", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) throwSynth("createSynthGraph(description): description is required", false);
        return makeSynthGraphValue(a[0]);
    });
    b.def("playSynth", 2, playSynthFn);
    b.def("releaseSynth", 1, [](Value, std::span<const Value> a) {
        Engine* e = existingAudioEngine();
        if (e && !a.empty()) e->releaseSynth(i32At(a, 0));
        return ev::undefined();
    });
}

// render() returns an AudioBuffer, so a realm with SynthGraph needs that
// class even without the rest of Web Audio (a worker).
void installSynthGraph() {
    installAudioBufferClass();
    installSynthGraphClass();
}

} // namespace broaudio::api
