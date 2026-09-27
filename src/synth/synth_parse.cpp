// Synthesis graph descriptions: JSON -> validated SynthGraphData.
//
// Every error names the path of the field at fault. Every number becomes a
// parameter named by its path without the "nodes"/"layers" containers
// ("body.cutoff", "bell.modes.1.decay", "tail.offset"), so a caller can
// override it per trigger. Each layer's nodes are put in evaluation order by
// a depth-first walk from the output through the inputs in slot order, which
// depends only on the wiring (not on declaration order), then laid out into a
// LayerPlan whose key is its shape.

#include "synth_plan.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace broaudio {

namespace {

using json = nlohmann::ordered_json;

[[noreturn]] void fail(const std::string& path, const std::string& msg, bool range = false)
{
    throw SynthGraphError(path.empty() ? msg : path + ": " + msg, range);
}

std::string fmtNum(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string typeName(const json& v)
{
    if (v.is_null()) return "null";
    if (v.is_boolean()) return "a boolean";
    if (v.is_number()) return "a number";
    if (v.is_string()) return "a string";
    if (v.is_array()) return "an array";
    return "an object";
}

bool validId(const std::string& s)
{
    if (s.empty() || s.size() > 64) return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

struct Range { float lo, hi; };
constexpr Range kAny{-1e6f, 1e6f};
constexpr Range kFreq{0.0f, 1e6f};
constexpr Range kTime{0.0f, 600.0f};

// A slot as parsed: a node reference or a parameter.
struct PSlot {
    std::string ref;
    int param = -1;
    std::string path;
};

struct PNode {
    std::string id;
    std::string path;
    SynthKind kind = SynthKind::Mul;
    uint8_t variant = 0;
    std::vector<PSlot> slots;     // plan order, gain last
    std::vector<int> kParams;     // node constants fed from params (fm feedback, mix weights)
    EnvDef env;
    ResDef res;
    CombDef comb;
    ImpDef imp;
    int count = 0;
};

class Parser {
public:
    explicit Parser(SynthGraphData& d) : d_(d) {}

    void graph(const json& root);

private:
    SynthGraphData& d_;
    std::vector<std::string> allIds_;

    int addParam(const std::string& name, float value, float jitter, float jitterAbs, Range r)
    {
        SynthParamInfo p;
        p.name = name;
        p.value = value;
        p.jitter = jitter;
        p.jitterAbs = jitterAbs;
        p.lo = r.lo;
        p.hi = r.hi;
        const int idx = static_cast<int>(d_.params.size());
        d_.params.push_back(p);
        d_.paramIndex[name] = idx;
        return idx;
    }

    // A number or {value, jitter, jitterAbs} at `path`, registered as `name`.
    int param(const json& v, const std::string& path, const std::string& name, Range r)
    {
        double value = 0, jitter = 0, jitterAbs = 0;
        if (v.is_number()) {
            value = v.get<double>();
        } else if (v.is_object()) {
            for (auto it = v.begin(); it != v.end(); ++it) {
                const std::string& key = it.key();
                if (key != "value" && key != "jitter" && key != "jitterAbs")
                    fail(path, "unknown field '" + key + "' (a parameter object has value, jitter, jitterAbs)");
                if (!it.value().is_number()) fail(path + "." + key, "must be a number, not " + typeName(it.value()));
            }
            if (!v.contains("value")) fail(path, "a parameter object needs a value");
            value = v["value"].get<double>();
            if (v.contains("jitter")) jitter = v["jitter"].get<double>();
            if (v.contains("jitterAbs")) jitterAbs = v["jitterAbs"].get<double>();
            if (!std::isfinite(jitter) || jitter < 0 || jitter > 1)
                fail(path + ".jitter", fmtNum(jitter) + " is out of range [0, 1] (a fraction of the value)", true);
            if (!std::isfinite(jitterAbs) || jitterAbs < 0 || jitterAbs > 1e6)
                fail(path + ".jitterAbs", fmtNum(jitterAbs) + " must be a finite number >= 0", true);
        } else {
            fail(path, "must be a number or {value, jitter, jitterAbs}, not " + typeName(v));
        }
        if (!std::isfinite(value)) fail(path, "must be finite", true);
        if (value < r.lo || value > r.hi)
            fail(path, fmtNum(value) + " is out of range [" + fmtNum(r.lo) + ", " + fmtNum(r.hi) + "]", true);
        return addParam(name, static_cast<float>(value), static_cast<float>(jitter),
                        static_cast<float>(jitterAbs), r);
    }

    // Optional numeric field with a default.
    int field(const json& obj, const PNode& n, const char* key, float def, Range r)
    {
        const std::string name = n.id + "." + key;
        if (obj.contains(key)) return param(obj[key], n.path + "." + key, name, r);
        return addParam(name, def, 0, 0, r);
    }

    int requiredField(const json& obj, const PNode& n, const char* key, Range r)
    {
        if (!obj.contains(key)) fail(n.path + "." + key, "required");
        return param(obj[key], n.path + "." + key, n.id + "." + key, r);
    }

    // A signal-capable input: a node id, a number or {value, ...}.
    PSlot slot(const json& obj, const PNode& n, const char* key, float def, Range r, bool required = false)
    {
        PSlot s;
        s.path = n.path + "." + key;
        if (obj.contains(key) && obj[key].is_string()) {
            s.ref = obj[key].get<std::string>();
            if (s.ref.empty()) fail(s.path, "a node id must not be empty");
            return s;
        }
        if (required && !obj.contains(key)) fail(s.path, "required (a node id)");
        s.param = field(obj, n, key, def, r);
        return s;
    }

    uint8_t variant(const json& obj, const PNode& n, const char* key,
                    std::initializer_list<const char*> names)
    {
        if (!obj.contains(key)) return 0;
        const json& v = obj[key];
        std::string all;
        uint8_t i = 0;
        for (const char* nm : names) {
            if (v.is_string() && v.get<std::string>() == nm) return i;
            all += (i ? ", " : "") + std::string(nm);
            ++i;
        }
        fail(n.path + "." + key, (v.is_string() ? "unknown value '" + v.get<std::string>() + "'" :
                                  "must be a string") + " (" + all + ")");
    }

    static void allowOnly(const json& obj, const std::string& path, const char* kind,
                          std::initializer_list<const char*> keys)
    {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            bool ok = it.key() == "type";
            for (const char* k : keys) ok = ok || it.key() == k;
            if (ok) continue;
            std::string all = "type";
            for (const char* k : keys) all += std::string(", ") + k;
            fail(path, "unknown field '" + it.key() + "' for a " + kind + " (fields: " + all + ")");
        }
    }

    SegCurve curve(const json& v, const std::string& path)
    {
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            if (s == "linear") return SegCurve::Linear;
            if (s == "exp") return SegCurve::Exp;
            if (s == "decay") return SegCurve::Decay;
        }
        fail(path, "must be 'linear', 'exp' or 'decay'");
    }

    void node(PNode& n, const json& obj);
    void env(PNode& n, const json& obj);
    void layer(LayerData& L, const std::string& id, const json& nodes, const json& output,
               const std::string& path);
};

void Parser::node(PNode& n, const json& obj)
{
    if (!obj.is_object()) fail(n.path, "a node must be an object, not " + typeName(obj));
    if (!obj.contains("type") || !obj["type"].is_string())
        fail(n.path + ".type", "required: 'osc', 'fm', 'noise', 'filter', 'env', 'sweep', 'shaper', "
                               "'resonator', 'comb', 'impulses', 'mul' or 'mix'");
    const std::string type = obj["type"].get<std::string>();
    auto gain = [&]() { return slot(obj, n, "gain", 1.0f, kAny); };
    if (type == "osc") {
        allowOnly(obj, n.path, "osc", {"wave", "freq", "pm", "pw", "gain"});
        n.kind = SynthKind::Osc;
        n.variant = variant(obj, n, "wave", {"sine", "saw", "square", "triangle"});
        n.slots = {slot(obj, n, "freq", 440.0f, kFreq), slot(obj, n, "pm", 0.0f, kAny),
                   slot(obj, n, "pw", 0.5f, {0.0f, 1.0f}), gain()};
    } else if (type == "fm") {
        allowOnly(obj, n.path, "fm", {"freq", "ratio", "index", "feedback", "gain"});
        n.kind = SynthKind::Fm;
        n.slots = {slot(obj, n, "freq", 440.0f, kFreq), slot(obj, n, "ratio", 1.0f, {0.0f, 1000.0f}),
                   slot(obj, n, "index", 1.0f, {0.0f, 1000.0f}), gain()};
        n.kParams = {field(obj, n, "feedback", 0.0f, {0.0f, 10.0f})};
    } else if (type == "noise") {
        allowOnly(obj, n.path, "noise", {"color", "gain"});
        n.kind = SynthKind::Noise;
        n.variant = variant(obj, n, "color", {"white", "pink", "brown"});
        n.slots = {gain()};
    } else if (type == "filter") {
        allowOnly(obj, n.path, "filter", {"mode", "input", "cutoff", "q", "gain"});
        n.kind = SynthKind::Filter;
        n.variant = variant(obj, n, "mode", {"lowpass", "highpass", "bandpass", "notch"});
        n.slots = {slot(obj, n, "input", 0.0f, kAny, true), slot(obj, n, "cutoff", 1000.0f, kFreq),
                   slot(obj, n, "q", 0.707f, {0.05f, 1000.0f}), gain()};
    } else if (type == "env" || type == "sweep") {
        n.kind = SynthKind::Env;
        env(n, obj);
        n.slots = {gain()};
    } else if (type == "shaper") {
        allowOnly(obj, n.path, "shaper", {"mode", "input", "drive", "gain"});
        n.kind = SynthKind::Shaper;
        n.variant = variant(obj, n, "mode", {"tanh", "clip", "fold"});
        n.slots = {slot(obj, n, "input", 0.0f, kAny, true), slot(obj, n, "drive", 1.0f, {0.0f, 1e4f}), gain()};
    } else if (type == "resonator") {
        allowOnly(obj, n.path, "resonator", {"input", "freq", "modes", "gain"});
        n.kind = SynthKind::Resonator;
        n.slots = {slot(obj, n, "input", 0.0f, kAny, true), gain()};
        n.res.freq = field(obj, n, "freq", 440.0f, {1.0f, 1e5f});
        const std::string mp = n.path + ".modes";
        if (obj.contains("modes")) {
            const json& ms = obj["modes"];
            if (!ms.is_array() || ms.empty() || ms.size() > static_cast<size_t>(kSynthMaxModes))
                fail(mp, "must be an array of 1 to " + std::to_string(kSynthMaxModes) + " modes");
            for (size_t i = 0; i < ms.size(); ++i) {
                const json& m = ms[i];
                const std::string path = mp + "." + std::to_string(i);
                if (!m.is_object()) fail(path, "a mode must be an object {ratio, decay, gain}");
                for (auto it = m.begin(); it != m.end(); ++it)
                    if (it.key() != "ratio" && it.key() != "decay" && it.key() != "gain")
                        fail(path, "unknown field '" + it.key() + "' (a mode has ratio, decay, gain)");
                const std::string name = n.id + ".modes." + std::to_string(i);
                auto modeField = [&](const char* key, float def, Range r) {
                    if (m.contains(key)) return param(m[key], path + "." + key, name + "." + key, r);
                    return addParam(name + "." + key, def, 0, 0, r);
                };
                ModeDef md;
                md.ratio = modeField("ratio", 1.0f, {0.0f, 1000.0f});
                md.decay = modeField("decay", 1.0f, {0.001f, 600.0f});
                md.gain = modeField("gain", 1.0f, {-1e3f, 1e3f});
                n.res.modes.push_back(md);
            }
        } else {
            ModeDef md;
            md.ratio = addParam(n.id + ".modes.0.ratio", 1.0f, 0, 0, {0.0f, 1000.0f});
            md.decay = addParam(n.id + ".modes.0.decay", 1.0f, 0, 0, {0.001f, 600.0f});
            md.gain = addParam(n.id + ".modes.0.gain", 1.0f, 0, 0, {-1e3f, 1e3f});
            n.res.modes.push_back(md);
        }
        n.count = static_cast<int>(n.res.modes.size());
    } else if (type == "comb") {
        allowOnly(obj, n.path, "comb", {"input", "freq", "feedback", "damp", "gain"});
        n.kind = SynthKind::Comb;
        n.slots = {slot(obj, n, "input", 0.0f, kAny, true), gain()};
        n.comb.freq = field(obj, n, "freq", 220.0f, {1.0f, 1e5f});
        n.comb.feedback = field(obj, n, "feedback", 0.7f, {-0.9999f, 0.9999f});
        n.comb.damp = field(obj, n, "damp", 0.0f, {0.0f, 1.0f});
    } else if (type == "impulses") {
        allowOnly(obj, n.path, "impulses", {"shape", "rate", "jitter", "length", "ampJitter", "gain"});
        n.kind = SynthKind::Impulses;
        n.variant = variant(obj, n, "shape", {"impulse", "rect", "hann", "decay"});
        n.slots = {slot(obj, n, "rate", 10.0f, {0.0f, 24000.0f}), gain()};
        n.kParams = {field(obj, n, "jitter", 0.0f, {0.0f, 1.0f}), field(obj, n, "ampJitter", 0.0f, {0.0f, 1.0f})};
        n.imp.length = field(obj, n, "length", 0.005f, {1e-4f, 10.0f});
    } else if (type == "mul") {
        allowOnly(obj, n.path, "mul", {"a", "b", "gain"});
        n.kind = SynthKind::Mul;
        n.slots = {slot(obj, n, "a", 0.0f, kAny, true), slot(obj, n, "b", 0.0f, kAny, true), gain()};
    } else if (type == "mix") {
        allowOnly(obj, n.path, "mix", {"inputs", "gain"});
        n.kind = SynthKind::Mix;
        const std::string ip = n.path + ".inputs";
        if (!obj.contains("inputs")) fail(ip, "required (an array of node ids, numbers or {node, weight})");
        const json& ins = obj["inputs"];
        if (!ins.is_array() || ins.empty() || ins.size() > static_cast<size_t>(kSynthMaxMixInputs))
            fail(ip, "must be an array of 1 to " + std::to_string(kSynthMaxMixInputs) + " inputs");
        for (size_t i = 0; i < ins.size(); ++i) {
            const json& e = ins[i];
            const std::string path = ip + "." + std::to_string(i);
            const std::string name = n.id + ".inputs." + std::to_string(i);
            PSlot s;
            s.path = path;
            int weight;
            if (e.is_string()) {
                s.ref = e.get<std::string>();
                weight = addParam(name + ".weight", 1.0f, 0, 0, kAny);
            } else if (e.is_object() && e.contains("node")) {
                for (auto it = e.begin(); it != e.end(); ++it)
                    if (it.key() != "node" && it.key() != "weight")
                        fail(path, "unknown field '" + it.key() + "' (a mix input is {node, weight})");
                if (!e["node"].is_string()) fail(path + ".node", "must be a node id");
                s.ref = e["node"].get<std::string>();
                weight = e.contains("weight") ? param(e["weight"], path + ".weight", name + ".weight", kAny)
                                              : addParam(name + ".weight", 1.0f, 0, 0, kAny);
            } else {
                s.param = param(e, path, name, kAny);
                weight = addParam(name + ".weight", 1.0f, 0, 0, kAny);
            }
            n.slots.push_back(s);
            n.kParams.push_back(weight);
        }
        n.count = static_cast<int>(ins.size());
        n.slots.push_back(gain());
    } else {
        fail(n.path + ".type", "unknown type '" + type + "' (osc, fm, noise, filter, env, sweep, shaper, "
                                                         "resonator, comb, impulses, mul, mix)");
    }
}

void Parser::env(PNode& n, const json& obj)
{
    EnvDef& e = n.env;
    const bool sweep = obj["type"].get<std::string>() == "sweep";
    if (sweep) {
        allowOnly(obj, n.path, "sweep", {"from", "to", "time", "delay", "curve", "gain"});
        e.amp = false;
        e.start = requiredField(obj, n, "from", kAny);
        const int to = requiredField(obj, n, "to", kAny);
        const int time = requiredField(obj, n, "time", kTime);
        const int delay = field(obj, n, "delay", 0.0f, kTime);
        const SegCurve c = obj.contains("curve") ? curve(obj["curve"], n.path + ".curve") : SegCurve::Exp;
        e.segs = {SegDef{delay, e.start, SegCurve::Linear}, SegDef{time, to, c}};
        return;
    }
    if (obj.contains("segments")) {
        allowOnly(obj, n.path, "segment env", {"start", "segments", "hold", "gate", "gain"});
        e.start = field(obj, n, "start", 0.0f, kAny);
        const json& segs = obj["segments"];
        const std::string sp = n.path + ".segments";
        if (!segs.is_array() || segs.empty() || segs.size() > static_cast<size_t>(kSynthMaxSegments))
            fail(sp, "must be an array of 1 to " + std::to_string(kSynthMaxSegments) + " {time, level, curve}");
        for (size_t i = 0; i < segs.size(); ++i) {
            const json& s = segs[i];
            const std::string path = sp + "." + std::to_string(i);
            const std::string name = n.id + ".segments." + std::to_string(i);
            if (!s.is_object()) fail(path, "a segment must be an object {time, level, curve}");
            for (auto it = s.begin(); it != s.end(); ++it)
                if (it.key() != "time" && it.key() != "level" && it.key() != "curve")
                    fail(path, "unknown field '" + it.key() + "' (a segment has time, level, curve)");
            if (!s.contains("time")) fail(path + ".time", "required");
            if (!s.contains("level")) fail(path + ".level", "required");
            SegDef d;
            d.time = param(s["time"], path + ".time", name + ".time", kTime);
            d.level = param(s["level"], path + ".level", name + ".level", kAny);
            d.curve = s.contains("curve") ? curve(s["curve"], path + ".curve") : SegCurve::Linear;
            e.segs.push_back(d);
        }
        if (obj.contains("hold")) {
            const json& h = obj["hold"];
            if (!h.is_number_integer() || h.get<int64_t>() < 0 ||
                h.get<int64_t>() >= static_cast<int64_t>(e.segs.size()))
                fail(n.path + ".hold", "must be a segment index, 0 to " + std::to_string(e.segs.size() - 1));
            e.hold = static_cast<int>(h.get<int64_t>());
        }
    } else {
        allowOnly(obj, n.path, "env", {"attack", "decay", "sustain", "release", "peak", "gate", "gain"});
        const int attack = field(obj, n, "attack", 0.005f, kTime);
        const int decay = field(obj, n, "decay", 0.1f, kTime);
        const int sustain = field(obj, n, "sustain", 1.0f, kAny);
        const int release = field(obj, n, "release", 0.1f, kTime);
        const int peak = field(obj, n, "peak", 1.0f, kAny);
        e.segs = {SegDef{attack, peak, SegCurve::Linear}, SegDef{decay, sustain, SegCurve::Decay},
                  SegDef{release, -1, SegCurve::Decay}};
        e.hold = 1;
    }
    if (obj.contains("gate")) e.gate = param(obj["gate"], n.path + ".gate", n.id + ".gate", {0.0f, 3600.0f});
}

void Parser::layer(LayerData& L, const std::string& id, const json& nodes, const json& output,
                   const std::string& path)
{
    const std::string np = path.empty() ? "nodes" : path + ".nodes";
    if (!nodes.is_object() || nodes.empty()) fail(np, "must be an object of nodes keyed by id");
    if (nodes.size() > static_cast<size_t>(kSynthMaxNodes))
        fail(np, "a layer holds at most " + std::to_string(kSynthMaxNodes) + " nodes");
    std::vector<PNode> pn;
    std::unordered_map<std::string, int> byId;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        PNode n;
        n.id = it.key();
        n.path = np + "." + n.id;
        if (!validId(n.id)) fail(n.path, "a node id is letters, digits and _, not starting with a digit");
        if (std::find(allIds_.begin(), allIds_.end(), n.id) != allIds_.end())
            fail(n.path, "id '" + n.id + "' is used twice (ids are unique across the graph)");
        allIds_.push_back(n.id);
        node(n, it.value());
        byId[n.id] = static_cast<int>(pn.size());
        pn.push_back(std::move(n));
    }
    const std::string op = path.empty() ? "output" : path + ".output";
    if (!output.is_string()) fail(op, "required: the id of the node the layer outputs");
    const std::string outId = output.get<std::string>();
    if (!byId.count(outId)) fail(op, "unknown node '" + outId + "'");

    // Resolve references.
    std::vector<std::vector<int>> wires(pn.size());
    for (size_t j = 0; j < pn.size(); ++j) {
        for (PSlot& s : pn[j].slots) {
            if (s.ref.empty()) { wires[j].push_back(-1); continue; }
            auto f = byId.find(s.ref);
            if (f == byId.end()) {
                const bool elsewhere = std::find(allIds_.begin(), allIds_.end(), s.ref) != allIds_.end();
                fail(s.path, "unknown node '" + s.ref + "'" +
                                 (elsewhere ? " (it is in another layer; inputs connect within a layer)" : ""));
            }
            wires[j].push_back(f->second);
        }
    }

    // Evaluation order: depth-first from the output, inputs in slot order.
    std::vector<int> order, state(pn.size(), 0), stack;
    std::function<void(int)> visit = [&](int j) {
        if (state[j] == 2) return;
        if (state[j] == 1) {
            std::string cyc;
            auto it = std::find(stack.begin(), stack.end(), j);
            for (; it != stack.end(); ++it) cyc += pn[*it].id + " -> ";
            fail(np, "cycle: " + cyc + pn[j].id + " (feedback is not supported; a comb node has it built in)");
        }
        state[j] = 1;
        stack.push_back(j);
        for (int w : wires[j])
            if (w >= 0) visit(w);
        stack.pop_back();
        state[j] = 2;
        order.push_back(j);
    };
    visit(byId[outId]);
    for (size_t j = 0; j < pn.size(); ++j)
        if (!state[j]) fail(pn[j].path, "node '" + pn[j].id + "' does not reach the output '" + outId + "'");
    std::vector<int> newIndex(pn.size());
    for (size_t o = 0; o < order.size(); ++o) newIndex[order[o]] = static_cast<int>(o);

    LayerPlan plan;
    for (int j : order) {
        const PNode& n = pn[j];
        NodePlan np2;
        np2.kind = n.kind;
        np2.variant = n.variant;
        np2.count = static_cast<uint8_t>(n.count);
        for (int w : wires[j]) {
            SlotPlan sp;
            if (w >= 0) sp.node = static_cast<int16_t>(newIndex[w]);
            np2.slots.push_back(sp);
        }
        plan.nodes.push_back(std::move(np2));
    }
    plan.output = newIndex[byId[outId]];
    layoutSynthPlan(plan);
    if (plan.kCount > kSynthMaxK || plan.words > kSynthMaxWords || plan.kiCount > kSynthMaxKi ||
        plan.bufCount > kSynthMaxBufs)
        fail(np, "the layer is too large (too many resonator modes, combs or inputs)");

    L.id = id;
    for (size_t o = 0; o < order.size(); ++o) {
        const PNode& n = pn[order[o]];
        const NodePlan& nd = plan.nodes[o];
        for (size_t q = 0; q < n.slots.size(); ++q)
            if (n.slots[q].param >= 0) L.feeds.emplace_back(nd.slots[q].k, n.slots[q].param);
        for (size_t q = 0; q < n.kParams.size(); ++q) L.feeds.emplace_back(nd.kBase + static_cast<int>(q), n.kParams[q]);
        const int ni = static_cast<int>(o);
        if (n.kind == SynthKind::Env) {
            EnvDef e = n.env;
            e.node = ni;
            if (e.amp) ++d_.ampEnvs;
            L.envs.push_back(e);
        } else if (n.kind == SynthKind::Resonator) {
            ResDef r = n.res;
            r.node = ni;
            L.res.push_back(r);
        } else if (n.kind == SynthKind::Comb) {
            CombDef c = n.comb;
            c.node = ni;
            L.combs.push_back(c);
        } else if (n.kind == SynthKind::Noise) {
            L.rngWords.push_back(nd.sBase);
        } else if (n.kind == SynthKind::Impulses) {
            ImpDef d = n.imp;
            d.node = ni;
            L.impulses.push_back(d);
        }
    }
    L.shape = internSynthShape(std::move(plan));
}

void Parser::graph(const json& root)
{
    if (!root.is_object()) fail("", "a synth graph must be an object, not " + typeName(root));
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& k = it.key();
        if (k != "nodes" && k != "output" && k != "layers" && k != "duration")
            fail("", "unknown field '" + k + "' (a graph has nodes + output, or layers; and duration)");
    }
    if (root.contains("duration"))
        d_.duration = param(root["duration"], "duration", "duration", {0.001f, 3600.0f});
    if (root.contains("layers")) {
        if (root.contains("nodes") || root.contains("output"))
            fail("", "a graph has either nodes + output or layers, not both");
        const json& layers = root["layers"];
        if (!layers.is_object() || layers.empty() || layers.size() > static_cast<size_t>(kSynthMaxLayers))
            fail("layers", "must be an object of 1 to " + std::to_string(kSynthMaxLayers) + " layers keyed by id");
        for (auto it = layers.begin(); it != layers.end(); ++it) {
            const std::string id = it.key();
            const std::string path = "layers." + id;
            if (!validId(id)) fail(path, "a layer id is letters, digits and _, not starting with a digit");
            const json& l = it.value();
            if (!l.is_object()) fail(path, "a layer must be an object {nodes, output, offset, gain}");
            for (auto f = l.begin(); f != l.end(); ++f)
                if (f.key() != "nodes" && f.key() != "output" && f.key() != "offset" && f.key() != "gain")
                    fail(path, "unknown field '" + f.key() + "' (a layer has nodes, output, offset, gain)");
            if (std::find(allIds_.begin(), allIds_.end(), id) != allIds_.end())
                fail(path, "id '" + id + "' is used twice (ids are unique across the graph)");
            allIds_.push_back(id);
            LayerData L;
            L.offset = l.contains("offset") ? param(l["offset"], path + ".offset", id + ".offset", {0.0f, 600.0f})
                                            : addParam(id + ".offset", 0.0f, 0, 0, {0.0f, 600.0f});
            L.gain = l.contains("gain") ? param(l["gain"], path + ".gain", id + ".gain", {-1e3f, 1e3f})
                                        : addParam(id + ".gain", 1.0f, 0, 0, {-1e3f, 1e3f});
            if (!l.contains("nodes")) fail(path + ".nodes", "required");
            layer(L, id, l["nodes"], l.contains("output") ? l["output"] : json(), path);
            d_.layers.push_back(std::move(L));
        }
    } else {
        if (!root.contains("nodes")) fail("nodes", "required (or layers)");
        LayerData L;
        layer(L, "main", root["nodes"], root.contains("output") ? root["output"] : json(), "");
        d_.layers.push_back(std::move(L));
    }
}

} // namespace

void layoutSynthPlan(LayerPlan& plan)
{
    int k = 2, ki = 0, bufs = 0, words = 0;
    plan.wordIsInt.clear();
    auto word = [&](bool isInt) {
        plan.wordIsInt.push_back(isInt ? 1 : 0);
        return words++;
    };
    std::string key = "v1";
    for (size_t j = 0; j < plan.nodes.size(); ++j) {
        NodePlan& n = plan.nodes[j];
        key += ';';
        key += static_cast<char>('A' + static_cast<int>(n.kind));
        key += std::to_string(n.variant) + "." + std::to_string(n.count) + "(";
        for (size_t q = 0; q < n.slots.size(); ++q) {
            SlotPlan& s = n.slots[q];
            if (q) key += ',';
            if (s.wired()) {
                key += std::to_string(s.node);
            } else {
                key += 'c';
                s.k = static_cast<int16_t>(k++);
            }
        }
        key += ')';
        n.kBase = n.kiBase = n.buf = -1;
        n.sBase = words;
        switch (n.kind) {
        case SynthKind::Osc: word(false); break;
        case SynthKind::Fm: n.kBase = k; k += 1; word(false); word(false); word(false); break;
        case SynthKind::Noise:
            word(true);
            if (n.variant == static_cast<uint8_t>(NoiseColor::Pink))
                for (int i = 0; i < 7; ++i) word(false);
            else if (n.variant == static_cast<uint8_t>(NoiseColor::Brown))
                word(false);
            break;
        case SynthKind::Filter: word(false); word(false); break;
        case SynthKind::Env: n.kBase = k; k += 2; word(false); break;
        case SynthKind::Shaper: break;
        case SynthKind::Resonator:
            n.kBase = k;
            k += 3 * n.count;
            for (int i = 0; i < 2 * n.count; ++i) word(false);
            break;
        case SynthKind::Comb:
            n.kBase = k;
            k += 3;
            n.kiBase = ki;
            ki += 2;
            n.buf = bufs++;
            word(true);
            word(false);
            break;
        case SynthKind::Mul: break;
        case SynthKind::Mix: n.kBase = k; k += n.count; break;
        case SynthKind::Impulses:
            n.kBase = k;
            k += 4;
            word(true);
            for (int i = 0; i < 5; ++i) word(false);
            break;
        }
        n.sCount = words - n.sBase;
    }
    key += "->" + std::to_string(plan.output);
    plan.kCount = k;
    plan.kiCount = ki;
    plan.bufCount = bufs;
    plan.words = words;
    plan.key = std::move(key);
}

std::vector<std::pair<int, float>> SynthGraph::overridesFromJson(std::string_view text) const
{
    json root;
    try {
        root = json::parse(text.begin(), text.end());
    } catch (const std::exception& e) {
        throw SynthGraphError(std::string("params: not valid JSON: ") + e.what());
    }
    std::vector<std::pair<int, float>> out;
    if (root.is_null()) return out;
    if (!root.is_object()) fail("params", "must be an object {name: number}");
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string path = "params." + it.key();
        const int idx = paramIndex(it.key());
        if (idx < 0) fail(path, "no parameter named '" + it.key() + "'");
        if (!it.value().is_number()) fail(path, "must be a number, not " + typeName(it.value()));
        const double v = it.value().get<double>();
        const SynthParamInfo& p = data_->params[idx];
        if (!std::isfinite(v) || v < p.lo || v > p.hi)
            fail(path, fmtNum(v) + " is out of range [" + fmtNum(p.lo) + ", " + fmtNum(p.hi) + "]", true);
        out.emplace_back(idx, static_cast<float>(v));
    }
    return out;
}

std::unique_ptr<SynthGraphData> parseSynthGraph(std::string_view text)
{
    json root;
    try {
        root = json::parse(text.begin(), text.end());
    } catch (const std::exception& e) {
        throw SynthGraphError(std::string("not valid JSON: ") + e.what());
    }
    auto d = std::make_unique<SynthGraphData>();
    Parser p(*d);
    p.graph(root);
    return d;
}

} // namespace broaudio
