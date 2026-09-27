// `bro.ear.fit(graph, opts)`: search a SynthGraph's parameters until its
// render meets a target (include/broaudio/ear/fit.h). This half reads the
// options, runs the synchronous form and builds the result; the external
// scorer's JS glue and the async form (opts.onDone) are in
// host_audio_ear_fit_job.cpp. Documented in bro's docs/ear-api.js.
//
// Errors are TypeErrors, or RangeErrors for a number out of range, whose
// message starts with "bro.ear.fit: " and the path of the option.

#include "host_audio_ear_fit.h"
#include "host_audio_internal.h"
#include "host_audio_synth_graph.h"

#include <broaudio/synth/synth_graph.h>

#include <initializer_list>
#include <sstream>
#include <stdexcept>

namespace broaudio::api {

namespace {

namespace ear = broaudio::ear;

[[noreturn]] void fail(const std::string& msg, bool range = false) {
    const std::string m = "bro.ear.fit: " + msg;
    if (range) ev::throwRangeError(m);
    ev::throwTypeError(m);
}

std::string fmt(double v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

Value callGlobalFn(const char* ns, const char* fn, Value arg) {
    ev::Persistent a(arg);
    ev::GlobalValue g = ev::globalValue(ns);
    if (!g.found || !ev::isObject(g.value)) fail(std::string(ns) + " is missing from this realm");
    ev::Persistent obj(g.value);
    ev::Persistent f(ev::getProperty(obj.get(), fn));
    const Value args[1] = {a.get()};
    ev::CallResult r = ev::call(f.get(), obj.get(), std::span<const Value>(args, 1));
    if (r.thrown) ev::throwValue(r.value);
    return r.value;
}

std::vector<std::string> keysOf(Value obj) {
    ev::Persistent keys(callGlobalFn("Object", "keys", obj));
    const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(keys.get(), "length")));
    std::vector<std::string> out;
    for (uint32_t i = 0; i < n; ++i) out.push_back(ev::toUtf8(ev::getElement(keys.get(), i)));
    return out;
}

bool isArray(Value v) { return ev::isObject(v) && ev::toBool(callGlobalFn("Array", "isArray", v)); }
bool isPlainObject(Value v) { return ev::isObject(v) && !ev::isFunction(v) && !ev::isTypedArray(v); }

// A rooted options object with typed readers. A missing (undefined) field
// leaves `out` alone; a present one of the wrong type throws.
struct Obj {
    ev::Persistent obj;
    std::string path;  // "" for the top level, else "measures.t60." etc.

    Obj(Value v, std::string p) : obj(v), path(std::move(p)) {}

    std::string at(std::string_view name) const { return path + std::string(name); }
    Value get(std::string_view name) const { return ev::getProperty(obj.get(), name); }
    bool has(std::string_view name) const { return !ev::isUndefined(get(name)); }

    void allow(std::initializer_list<std::string_view> allowed) const {
        for (const std::string& k : keysOf(obj.get())) {
            bool ok = false;
            for (std::string_view a : allowed) ok = ok || a == k;
            if (ok) continue;
            std::string list;
            for (std::string_view a : allowed) list += (list.empty() ? "" : ", ") + std::string(a);
            fail((path.empty() ? "" : path.substr(0, path.size() - 1) + ": ") + "unknown option '" + k +
                 "' (expected " + list + ")");
        }
    }
    bool number(std::string_view name, double& out, double lo = -1e300, double hi = 1e300) const {
        if (!has(name)) return false;
        Value v = get(name);
        if (!ev::isNumber(v)) fail(at(name) + " must be a number");
        const double d = ev::toDouble(v);
        if (!(d >= lo && d <= hi)) fail(at(name) + " is " + fmt(d) + ", outside [" + fmt(lo) + ", " + fmt(hi) + "]", true);
        out = d;
        return true;
    }
    bool integer(std::string_view name, int& out, int lo, int hi) const {
        double d = 0;
        if (!number(name, d, lo, hi)) return false;
        if (d != std::floor(d)) fail(at(name) + " must be an integer", true);
        out = static_cast<int>(d);
        return true;
    }
    bool boolean(std::string_view name, bool& out) const {
        if (!has(name)) return false;
        Value v = get(name);
        if (!ev::isBool(v)) fail(at(name) + " must be a boolean");
        out = ev::toBool(v);
        return true;
    }
    bool string(std::string_view name, std::string& out) const {
        if (!has(name)) return false;
        Value v = get(name);
        if (!ev::isString(v)) fail(at(name) + " must be a string");
        out = ev::toUtf8(v);
        return true;
    }
    // A nested plain object, or false when absent.
    bool object(std::string_view name, ev::Persistent& out) const {
        if (!has(name)) return false;
        Value v = get(name);
        if (!isPlainObject(v)) fail(at(name) + " must be an object");
        out.set(v);
        return true;
    }
};

void readParamSpec(const Obj& o, ear::FitParamSpec& s) {
    o.number("min", s.min);
    o.number("max", s.max);
    o.number("start", s.start);
    std::string scale;
    if (o.string("scale", scale)) {
        if (scale == "log") s.scale = ear::FitScale::Log;
        else if (scale == "linear") s.scale = ear::FitScale::Linear;
        else if (scale == "auto") s.scale = ear::FitScale::Auto;
        else fail(o.at("scale") + " must be 'log', 'linear' or 'auto'");
    }
}

void readParams(Value v, ear::FitOptions& fo) {
    ev::Persistent root(v);
    if (isArray(root.get())) {
        const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(root.get(), "length")));
        for (uint32_t i = 0; i < n; ++i) {
            const std::string path = "params." + std::to_string(i);
            ev::Persistent e(ev::getElement(root.get(), i));
            ear::FitParamSpec s;
            if (ev::isString(e.get())) {
                s.name = ev::toUtf8(e.get());
            } else if (isPlainObject(e.get())) {
                Obj o(e.get(), path + ".");
                o.allow({"name", "min", "max", "scale", "start"});
                if (!o.string("name", s.name)) fail(path + ".name is required");
                readParamSpec(o, s);
            } else {
                fail(path + " must be a parameter name or {name, min?, max?, scale?, start?}");
            }
            fo.params.push_back(std::move(s));
        }
        return;
    }
    if (!isPlainObject(root.get())) fail("params must be an array of names or an object {name: true | {min, max, scale, start}}");
    for (const std::string& k : keysOf(root.get())) {
        ev::Persistent e(ev::getProperty(root.get(), k));
        ear::FitParamSpec s;
        s.name = k;
        if (ev::isBool(e.get()) && ev::toBool(e.get())) {
            // searched with the defaults
        } else if (isPlainObject(e.get())) {
            Obj o(e.get(), "params." + k + ".");
            o.allow({"min", "max", "scale", "start"});
            readParamSpec(o, s);
        } else {
            fail("params." + k + " must be true or {min?, max?, scale?, start?}");
        }
        fo.params.push_back(std::move(s));
    }
}

void readFixed(Value v, const SynthGraph& g, ear::FitOptions& fo) {
    ev::Persistent root(v);
    if (isArray(root.get())) {
        const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(root.get(), "length")));
        for (uint32_t i = 0; i < n; ++i) {
            Value e = ev::getElement(root.get(), i);
            if (!ev::isString(e)) fail("fixed." + std::to_string(i) + " must be a parameter name");
            const std::string name = ev::toUtf8(e);
            const int idx = g.paramIndex(name);
            if (idx < 0) fail("fixed." + name + ": unknown parameter");
            fo.fixed.push_back({name, g.params()[idx].value});
        }
        return;
    }
    if (!isPlainObject(root.get())) fail("fixed must be an object {name: value} or an array of names");
    Obj o(root.get(), "fixed.");
    for (const std::string& k : keysOf(root.get())) {
        double d = 0;
        if (!o.number(k, d)) fail("fixed." + k + " must be a number");
        fo.fixed.push_back({k, d});
    }
}

void readMeasures(Value v, ear::FitOptions& fo) {
    if (!isPlainObject(v)) fail("measures must be an object {name: target | {value, weight?, scale?}}");
    Obj o(v, "measures.");
    for (const std::string& k : keysOf(o.obj.get())) {
        ear::FitMeasureTarget t;
        t.name = k;
        ev::Persistent e(o.get(k));
        if (ev::isNumber(e.get())) {
            t.value = ev::toDouble(e.get());
        } else if (isPlainObject(e.get())) {
            Obj m(e.get(), "measures." + k + ".");
            m.allow({"value", "weight", "scale"});
            if (!m.number("value", t.value)) fail("measures." + k + ".value is required");
            m.number("weight", t.weight, 0.0, 1e9);
            m.number("scale", t.scale, 1e-12, 1e12);
        } else {
            fail("measures." + k + " must be a number or {value, weight?, scale?}");
        }
        fo.measures.push_back(std::move(t));
    }
}

void readBands(Value v, ear::FitOptions& fo) {
    ev::Persistent root(v);
    if (!isArray(root.get())) fail("bands must be an array of {minHz, maxHz, db, weight?}");
    const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(root.get(), "length")));
    for (uint32_t i = 0; i < n; ++i) {
        const std::string path = "bands." + std::to_string(i);
        ev::Persistent e(ev::getElement(root.get(), i));
        if (!isPlainObject(e.get())) fail(path + " must be {minHz, maxHz, db, weight?}");
        Obj b(e.get(), path + ".");
        b.allow({"minHz", "maxHz", "db", "weight"});
        ear::FitBandTarget t;
        if (!b.number("minHz", t.minHz, 0.0, 1e6) || !b.number("maxHz", t.maxHz, 0.0, 1e6) || !b.number("db", t.db)) {
            fail(path + " needs minHz, maxHz and db");
        }
        b.number("weight", t.weight, 0.0, 1e9);
        fo.bands.push_back(t);
    }
}

void readCompare(Value v, ear::CompareOptions& c) {
    if (!isPlainObject(v)) fail("compare must be an object {align?, maxShift?, weights?}");
    Obj o(v, "compare.");
    o.allow({"align", "maxShift", "weights"});
    o.boolean("align", c.align);
    o.number("maxShift", c.maxShift, 0.0, 10.0);
    ev::Persistent w;
    if (o.object("weights", w)) {
        Obj wo(w.get(), "compare.weights.");
        wo.allow({"envelope", "spectrum", "tonality"});
        wo.number("envelope", c.envelopeWeight, 0.0, 1e9);
        wo.number("spectrum", c.spectrumWeight, 0.0, 1e9);
        wo.number("tonality", c.tonalityWeight, 0.0, 1e9);
    }
}

void readSeeds(const Obj& o, ear::FitOptions& fo) {
    if (!o.has("seeds")) return;
    ev::Persistent v(o.get("seeds"));
    fo.seeds.clear();
    if (ev::isNumber(v.get())) {
        int n = 0;
        o.integer("seeds", n, 1, 64);
        for (int i = 1; i <= n; ++i) fo.seeds.push_back(static_cast<uint32_t>(i));
        return;
    }
    if (!isArray(v.get())) fail("seeds must be a count (1..64) or an array of seeds");
    const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(v.get(), "length")));
    if (n < 1 || n > 64) fail("seeds must hold 1..64 seeds", true);
    for (uint32_t i = 0; i < n; ++i) {
        Value e = ev::getElement(v.get(), i);
        const double d = ev::isNumber(e) ? ev::toDouble(e) : -1.0;
        if (!(d >= 0 && d <= 4294967295.0 && d == std::floor(d))) {
            fail("seeds." + std::to_string(i) + " must be an integer in [0, 4294967295]", true);
        }
        fo.seeds.push_back(static_cast<uint32_t>(d));
    }
}

// opts.clap: {model, prompts, index?, weight?, options?}. String prompts are
// embedded once here (model.embedText) when the model can, so every clip is
// scored against cached embeddings.
void readClap(Value v, JsScoring& sc) {
    if (!isPlainObject(v)) fail("clap must be {model, prompts, index?, weight?, options?}");
    Obj o(v, "clap.");
    o.allow({"model", "prompts", "index", "weight", "options"});
    ev::Persistent model(o.get("model"));
    if (!ev::isObject(model.get()) || !ev::isFunction(ev::getProperty(model.get(), "score"))) {
        fail("clap.model must be a CLAP model (bro.ear.loadClap's), with a score() method");
    }
    ev::Persistent prompts(o.get("prompts"));
    if (!isArray(prompts.get()) || ev::toDouble(ev::getProperty(prompts.get(), "length")) < 1) {
        fail("clap.prompts must be a non-empty array of prompts (strings or embedText embeddings)");
    }
    const int n = static_cast<int>(ev::toDouble(ev::getProperty(prompts.get(), "length")));
    o.integer("index", sc.clapIndex, 0, n - 1);
    o.number("weight", sc.clapWeight, 0.0, 1e9);
    if (o.has("options")) sc.clapOptions.set(o.get("options"));
    bool anyString = false;
    for (int i = 0; i < n; ++i) anyString = anyString || ev::isString(ev::getElement(prompts.get(), i));
    ev::Persistent embed(ev::getProperty(model.get(), "embedText"));
    if (anyString && ev::isFunction(embed.get())) {
        // embedText takes strings only: embed those, keep given embeddings.
        ev::Persistent out(ev::makeArray(static_cast<uint32_t>(n)));
        for (int i = 0; i < n; ++i) {
            ev::Persistent e(ev::getElement(prompts.get(), i));
            if (ev::isString(e.get())) {
                const Value args[1] = {e.get()};
                ev::CallResult r = ev::call(embed.get(), model.get(), std::span<const Value>(args, 1));
                if (r.thrown) ev::throwValue(r.value);
                e.set(r.value);
            }
            out.set(ev::setElement(out.get(), static_cast<uint32_t>(i), e.get()));
        }
        prompts.set(out.get());
    }
    sc.clapModel.set(model.get());
    sc.clapPrompts.set(prompts.get());
    sc.hasClap = true;
}

void readTargets(const Obj& o, FitRequest& req, JsScoring& sc, double rate) {
    ear::FitOptions& fo = req.options;
    if (o.has("reference")) {
        req.reference = std::make_unique<ear::Clip>();
        ev::Persistent ref(o.get("reference"));
        readEarClip(ref.get(), rate, "bro.ear.fit: reference", *req.reference);
        fo.reference = req.reference.get();
    }
    ev::Persistent sub;
    if (o.has("compare")) readCompare(o.get("compare"), fo.compare);
    if (o.has("measures")) readMeasures(o.get("measures"), fo);
    if (o.has("bands")) readBands(o.get("bands"), fo);
    if (o.has("scorer")) {
        Value f = o.get("scorer");
        if (!ev::isFunction(f)) fail("scorer must be a function (clips) => distances");
        sc.fn.set(f);
        sc.hasFn = true;
    }
    if (o.has("clap")) readClap(o.get("clap"), sc);
    if (o.object("weights", sub)) {
        Obj w(sub.get(), "weights.");
        w.allow({"reference", "scorer", "clap"});
        w.number("reference", fo.referenceWeight, 0.0, 1e9);
        w.number("scorer", sc.fnWeight, 0.0, 1e9);
        double clap = sc.clapWeight;
        if (w.number("clap", clap, 0.0, 1e9)) sc.clapWeight = clap;
    }
}

void setNum(ObjectBuilder& b, std::string_view name, double d) {
    if (std::isnan(d)) b.set(name, ev::null());
    else b.set(name, ev::fromDouble(d));
}

Value paramsObject(const std::vector<ear::FitParamInfo>& params, const std::vector<double>& values) {
    ObjectBuilder o;
    for (size_t i = 0; i < params.size() && i < values.size(); ++i) o.set(params[i].name, values[i]);
    return o.get();
}

Value fitFn(Value, std::span<const Value> a) {
    if (a.size() < 2) fail("bro.ear.fit(graph, opts): a graph and options are required");
    ev::Persistent graphV(a[0]), optsV(a[1]);
    auto req = std::make_unique<FitRequest>();
    req->graph = synthGraphFromValue(graphV.get());
    if (!isPlainObject(optsV.get())) fail("options must be an object");
    Obj o(optsV.get(), "");
    o.allow({"params", "fixed", "reference", "compare", "measures", "bands", "scorer", "clap", "weights",
             "maxEvaluations", "maxSeconds", "stopAt", "population", "sigma", "seed", "threads", "restarts",
             "jitter", "seeds", "sampleRate", "maxDuration", "compiled", "loop", "onProgress", "onDone"});
    ear::FitOptions& fo = req->options;
    if (o.has("params")) readParams(o.get("params"), fo);
    if (o.has("fixed")) readFixed(o.get("fixed"), *req->graph, fo);
    double rate = 0;
    if (o.number("sampleRate", rate, 8000.0, 384000.0)) fo.sampleRate = static_cast<int>(rate);
    JsScoring sc;
    readTargets(o, *req, sc, rate);
    o.integer("maxEvaluations", fo.maxEvaluations, 1, 100000000);
    o.number("maxSeconds", fo.maxSeconds, 0.0, 1e7);
    o.number("stopAt", fo.stopAt);
    o.integer("population", fo.population, 0, 4096);
    o.number("sigma", fo.sigma, 1e-6, 1.0);
    double seed = 0;
    if (o.number("seed", seed, 0.0, 9007199254740991.0)) {
        if (seed != std::floor(seed)) fail("seed must be an integer", true);
        fo.seed = static_cast<uint64_t>(seed);
    }
    o.integer("threads", fo.threads, 0, 256);
    o.boolean("restarts", fo.restarts);
    o.boolean("jitter", fo.jitter);
    readSeeds(o, fo);
    o.number("maxDuration", fo.maxDuration, 0.0, 600.0);
    o.boolean("compiled", fo.compiled);
    if (o.has("loop")) {
        bool loopRange = false;
        const std::string loopErr = readSynthLoopOptions(o.get("loop"), fo.loop, loopRange);
        if (!loopErr.empty()) fail(loopErr, loopRange);
    }
    ev::Persistent onDone(o.get("onDone")), onProgress(o.get("onProgress"));
    if (!ev::isUndefined(onDone.get()) && !ev::isFunction(onDone.get())) fail("onDone must be a function");
    if (!ev::isUndefined(onProgress.get()) && !ev::isFunction(onProgress.get())) fail("onProgress must be a function");

    std::string err;
    bool range = false;
    try {
        req->plan = ear::planFit(req->graph, fo, sc.any());
    } catch (const std::out_of_range& e) {
        err = e.what();
        range = true;
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!err.empty()) fail(err, range);

    if (ev::isFunction(onDone.get())) return launchEarFit(std::move(req), std::move(sc), onDone.get(), onProgress.get());

    // Synchronous: the scorer and onProgress run right here, between
    // generations; a throw from either stops the fit and is rethrown.
    ev::Persistent thrown;
    bool failed = false;
    ear::FitScorer scorer;
    if (sc.any()) {
        scorer = [&](const std::vector<const ear::Clip*>& clips, std::vector<double>& out) {
            if (scoreBatchOnJsThread(sc, clips, out, err, thrown)) return true;
            failed = true;
            return false;
        };
    }
    ear::FitProgressFn progress;
    if (ev::isFunction(onProgress.get())) {
        progress = [&](const ear::FitProgress& p) {
            ev::Persistent pv(fitProgressValue(p, *p.bestValues, *p.params));
            const Value args[1] = {pv.get()};
            ev::CallResult r = ev::call(onProgress.get(), ev::undefined(), std::span<const Value>(args, 1));
            if (r.thrown) {
                thrown.set(r.value);
                err = "onProgress threw";
                failed = true;
                return false;
            }
            return !(ev::isBool(r.value) && !ev::toBool(r.value));
        };
    }
    ear::FitResult res;
    try {
        res = ear::fit(req->graph, fo, scorer, progress);
    } catch (const std::exception& e) {
        err = e.what();
        failed = true;
    }
    if (failed) {
        if (!ev::isUndefined(thrown.get())) ev::throwValue(thrown.get());
        fail(err);
    }
    return fitResultValue(res, *req);
}

} // namespace

Value fitProgressValue(const ear::FitProgress& p, const std::vector<double>& values,
                       const std::vector<ear::FitParamInfo>& params) {
    ObjectBuilder o;
    o.set("generation", static_cast<double>(p.generation));
    o.set("evaluations", static_cast<double>(p.evaluations));
    o.set("restarts", static_cast<double>(p.restarts));
    o.set("best", p.best);
    o.set("generationBest", p.generationBest);
    o.set("sigma", p.sigma);
    o.set("seconds", p.seconds);
    ev::Persistent pv(paramsObject(params, values));
    o.set("params", pv.get());
    return o.get();
}

Value fitResultValue(const ear::FitResult& r, const FitRequest& req) {
    const auto& infos = req.graph->params();
    ObjectBuilder out;
    {
        // Every override that reproduces the clip, searched and fixed.
        ObjectBuilder p;
        for (const auto& [idx, v] : r.overrides) p.set(infos[idx].name, static_cast<double>(v));
        out.set("params", p.get());
    }
    {
        ev::Persistent arr(hostArrayOf(r.params.size(), [&](size_t i) {
            const ear::FitParamInfo& pi = r.params[i];
            ObjectBuilder o;
            o.set("name", pi.name);
            o.set("value", i < r.values.size() ? r.values[i] : pi.start);
            o.set("min", pi.min);
            o.set("max", pi.max);
            o.set("scale", pi.log ? "log" : "linear");
            o.set("start", pi.start);
            return o.get();
        }));
        out.set("searched", arr.get());
    }
    out.set("distance", r.distance);
    {
        ObjectBuilder t;
        setNum(t, "reference", r.terms.reference);
        if (!std::isnan(r.terms.reference)) {
            ev::Persistent c(earComparisonValue(r.terms.comparison));
            t.set("comparison", c.get());
        }
        ObjectBuilder ms;
        for (const ear::FitMeasureResult& m : r.terms.measures) {
            ObjectBuilder mo;
            mo.set("target", m.target);
            setNum(mo, "measured", m.measured);
            mo.set("distance", m.distance);
            ms.set(m.name, mo.get());
        }
        t.set("measures", ms.get());
        ev::Persistent bands(hostArrayOf(r.terms.bands.size(), [&](size_t i) {
            const ear::FitBandResult& b = r.terms.bands[i];
            ObjectBuilder bo;
            bo.set("minHz", b.minHz);
            bo.set("maxHz", b.maxHz);
            bo.set("target", b.target);
            setNum(bo, "measured", b.measured);
            bo.set("distance", b.distance);
            return bo.get();
        }));
        t.set("bands", bands.get());
        setNum(t, "external", r.terms.scorer);
        out.set("terms", t.get());
    }
    out.set("evaluations", static_cast<double>(r.evaluations));
    out.set("generations", static_cast<double>(r.generations));
    out.set("restarts", static_cast<double>(r.restarts));
    out.set("population", static_cast<double>(r.population));
    out.set("seconds", r.seconds);
    out.set("stop", r.stop);
    {
        ev::Persistent arr(hostArrayOf(r.history.size(), [&](size_t i) {
            const ear::FitHistoryEntry& h = r.history[i];
            ObjectBuilder o;
            o.set("generation", static_cast<double>(h.generation));
            o.set("evaluations", static_cast<double>(h.evaluations));
            o.set("best", h.best);
            o.set("generationBest", h.generationBest);
            o.set("sigma", h.sigma);
            o.set("seconds", h.seconds);
            return o.get();
        }));
        out.set("history", arr.get());
    }
    {
        ev::Persistent clip(earClipValue(r.clip));
        out.set("clip", clip.get());
    }
    // What g.render() needs to reproduce `clip`: {params, seed, jitter,
    // sampleRate, maxDuration}.
    {
        ObjectBuilder rr;
        ev::Persistent p(ev::getProperty(out.get(), "params"));
        rr.set("params", p.get());
        rr.set("seed", static_cast<double>(req.options.seeds.empty() ? 1u : req.options.seeds[0]));
        rr.set("jitter", req.options.jitter);
        rr.set("sampleRate", static_cast<double>(r.sampleRate));
        rr.set("maxDuration", r.maxDuration);
        if (req.options.loop.enabled()) {
            ev::Persistent loop(synthLoopOptionsValue(req.options.loop));
            rr.set("loop", loop.get());
        }
        out.set("render", rr.get());
    }
    return out.get();
}

void registerEarFit(ObjectBuilder& earObj) { earObj.def("fit", 2, fitFn); }

} // namespace broaudio::api
