// Preset (de)serialization on AudioContext: plain JS objects <-> broaudio's
// preset structs (VoicePreset, BusPreset, ModPreset, EnginePreset).
//
// broaudio's toJson/fromJson operate on the structs, and the engine has no
// getter that reads its live state back into one, so the JS surface marshals
// a JS object into a struct (for *PresetToJson / apply*Preset) and a struct
// back into a JS object (for *PresetFromJson). Missing fields keep the
// struct's defaults; enums round-trip as the same lowercase strings the rest
// of the binding uses (waveform, filter type, distortion mode, LFO shape,
// mod source/dest, effect-order slot names).
//
// GC discipline (embed.h): every object handed in is rooted in a Persistent
// before its properties are read, because a property read may allocate and
// move everything else; nested objects are rooted the same way.

#include "host_audio_internal.h"

namespace broaudio::api {

namespace {

// ---- JS object field readers (default when absent) -------------------------

bool present(Value v) { return !ev::isUndefined(v) && !ev::isNull(v); }

double objNum(const ev::Persistent& o, const char* k, double def) {
    Value v = ev::getProperty(o.get(), k);
    if (!present(v) || ev::isObject(v)) return def;
    double d = ev::toDouble(v);
    return std::isnan(d) ? def : d;
}
float objF(const ev::Persistent& o, const char* k, float def) {
    return static_cast<float>(objNum(o, k, def));
}
bool objBool(const ev::Persistent& o, const char* k, bool def) {
    Value v = ev::getProperty(o.get(), k);
    return present(v) ? ev::toBool(v) : def;
}
int objInt(const ev::Persistent& o, const char* k, int def) {
    return saturateI32(objNum(o, k, def));
}
std::string objStr(const ev::Persistent& o, const char* k, const char* def) {
    Value v = ev::getProperty(o.get(), k);
    if (!present(v) || ev::isObject(v)) return def;
    return ev::toUtf8(v);
}
// A nested object property, or an undefined Persistent when it is not one.
ev::Persistent objChild(const ev::Persistent& o, const char* k) {
    Value v = ev::getProperty(o.get(), k);
    return ev::isObject(v) ? ev::Persistent(v) : ev::Persistent();
}
uint32_t arrayLen(const ev::Persistent& arr) {
    if (!ev::isObject(arr.get())) return 0;
    Value l = ev::getProperty(arr.get(), "length");
    if (!present(l) || ev::isObject(l)) return 0;
    double d = ev::toDouble(l);
    return saturateU32(d);
}
ev::Persistent arrayChild(const ev::Persistent& arr, uint32_t i) {
    Value v = ev::getElement(arr.get(), i);
    return ev::isObject(v) ? ev::Persistent(v) : ev::Persistent();
}

// ---- JS object -> preset struct ------------------------------------------

broaudio::VoicePreset toVoicePreset(const ev::Persistent& o) {
    broaudio::VoicePreset p;
    if (!ev::isObject(o.get())) return p;
    p.waveform          = parseWaveform(objStr(o, "waveform", "sine"));
    p.frequency         = objF(o, "frequency", p.frequency);
    p.gain              = objF(o, "gain", p.gain);
    p.pan               = objF(o, "pan", p.pan);
    p.pitchBend         = objF(o, "pitchBend", p.pitchBend);
    p.attackTime        = objF(o, "attackTime", p.attackTime);
    p.decayTime         = objF(o, "decayTime", p.decayTime);
    p.sustainLevel      = objF(o, "sustainLevel", p.sustainLevel);
    p.releaseTime       = objF(o, "releaseTime", p.releaseTime);
    p.filterEnabled     = objBool(o, "filterEnabled", p.filterEnabled);
    p.filterType        = parseFilterType(objStr(o, "filterType", "lowpass"));
    p.filterFreq        = objF(o, "filterFreq", p.filterFreq);
    p.filterQ           = objF(o, "filterQ", p.filterQ);
    p.unisonCount       = objInt(o, "unisonCount", p.unisonCount);
    p.unisonDetune      = objF(o, "unisonDetune", p.unisonDetune);
    p.unisonStereoWidth = objF(o, "unisonStereoWidth", p.unisonStereoWidth);
    return p;
}

broaudio::FilterPreset toFilterPreset(const ev::Persistent& o) {
    broaudio::FilterPreset p;
    if (!ev::isObject(o.get())) return p;
    p.enabled   = objBool(o, "enabled", p.enabled);
    p.type      = parseFilterType(objStr(o, "type", "lowpass"));
    p.frequency = objF(o, "frequency", p.frequency);
    p.Q         = objF(o, "Q", p.Q);
    p.gainDB    = objF(o, "gainDB", p.gainDB);
    return p;
}

broaudio::DelayPreset toDelayPreset(const ev::Persistent& o) {
    broaudio::DelayPreset p;
    if (!ev::isObject(o.get())) return p;
    p.enabled  = objBool(o, "enabled", p.enabled);
    p.time     = objF(o, "time", p.time);
    p.feedback = objF(o, "feedback", p.feedback);
    p.mix      = objF(o, "mix", p.mix);
    return p;
}

broaudio::CompressorPreset toCompressorPreset(const ev::Persistent& o) {
    broaudio::CompressorPreset p;
    if (!ev::isObject(o.get())) return p;
    p.enabled   = objBool(o, "enabled", p.enabled);
    p.threshold = objF(o, "threshold", p.threshold);
    p.ratio     = objF(o, "ratio", p.ratio);
    p.attackMs  = objF(o, "attackMs", p.attackMs);
    p.releaseMs = objF(o, "releaseMs", p.releaseMs);
    return p;
}

broaudio::ReverbPreset toReverbPreset(const ev::Persistent& o) {
    broaudio::ReverbPreset p;
    if (!ev::isObject(o.get())) return p;
    p.enabled  = objBool(o, "enabled", p.enabled);
    p.roomSize = objF(o, "roomSize", p.roomSize);
    p.damping  = objF(o, "damping", p.damping);
    p.mix      = objF(o, "mix", p.mix);
    return p;
}

broaudio::ChorusPreset toChorusPreset(const ev::Persistent& o) {
    broaudio::ChorusPreset p;
    if (!ev::isObject(o.get())) return p;
    p.enabled   = objBool(o, "enabled", p.enabled);
    p.rate      = objF(o, "rate", p.rate);
    p.depth     = objF(o, "depth", p.depth);
    p.mix       = objF(o, "mix", p.mix);
    p.feedback  = objF(o, "feedback", p.feedback);
    p.baseDelay = objF(o, "baseDelay", p.baseDelay);
    return p;
}

broaudio::DistortionPreset toDistortionPreset(const ev::Persistent& o) {
    broaudio::DistortionPreset p;
    if (!ev::isObject(o.get())) return p;
    p.enabled    = objBool(o, "enabled", p.enabled);
    p.mode       = parseDistortionMode(objStr(o, "mode", "softclip"));
    p.drive      = objF(o, "drive", p.drive);
    p.mix        = objF(o, "mix", p.mix);
    p.outputGain = objF(o, "outputGain", p.outputGain);
    p.crushBits  = objF(o, "crushBits", p.crushBits);
    p.crushRate  = objF(o, "crushRate", p.crushRate);
    return p;
}

broaudio::EqPreset toEqPreset(const ev::Persistent& o) {
    broaudio::EqPreset p;
    if (!ev::isObject(o.get())) return p;
    p.enabled    = objBool(o, "enabled", p.enabled);
    p.masterGain = objF(o, "masterGain", p.masterGain);
    ev::Persistent bg = objChild(o, "bandGains");
    if (ev::isObject(bg.get())) {
        for (uint32_t i = 0; i < 7; i++) {
            Value e = ev::getElement(bg.get(), i);
            if (present(e) && !ev::isObject(e)) {
                double d = ev::toDouble(e);
                if (!std::isnan(d)) p.bandGains[i] = static_cast<float>(d);
            }
        }
    }
    return p;
}

broaudio::BusPreset toBusPreset(const ev::Persistent& o) {
    broaudio::BusPreset p;
    if (!ev::isObject(o.get())) return p;
    p.gain = objF(o, "gain", p.gain);
    p.pan  = objF(o, "pan", p.pan);

    const uint32_t slotCount = static_cast<uint32_t>(broaudio::EffectSlot::Count);
    ev::Persistent ord = objChild(o, "effectOrder");
    if (ev::isObject(ord.get())) {
        for (uint32_t i = 0; i < slotCount; i++) {
            Value e = ev::getElement(ord.get(), i);
            if (present(e) && !ev::isObject(e)) {
                p.effectOrder[i] = parseEffectSlot(ev::toUtf8(e), p.effectOrder[i]);
            }
        }
    }

    ev::Persistent filt = objChild(o, "filters");
    if (ev::isObject(filt.get())) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(broaudio::BusPreset::MAX_FILTERS); i++) {
            ev::Persistent e = arrayChild(filt, i);
            if (ev::isObject(e.get())) p.filters[i] = toFilterPreset(e);
        }
    }

    p.delay      = toDelayPreset(objChild(o, "delay"));
    p.compressor = toCompressorPreset(objChild(o, "compressor"));
    p.reverb     = toReverbPreset(objChild(o, "reverb"));
    p.chorus     = toChorusPreset(objChild(o, "chorus"));
    p.distortion = toDistortionPreset(objChild(o, "distortion"));
    p.eq         = toEqPreset(objChild(o, "eq"));
    return p;
}

broaudio::LfoPreset toLfoPreset(const ev::Persistent& o) {
    broaudio::LfoPreset p;
    if (!ev::isObject(o.get())) return p;
    p.shape   = parseLfoShape(objStr(o, "shape", "sine"));
    p.rate    = objF(o, "rate", p.rate);
    p.depth   = objF(o, "depth", p.depth);
    p.offset  = objF(o, "offset", p.offset);
    p.bipolar = objBool(o, "bipolar", p.bipolar);
    p.sync    = objBool(o, "sync", p.sync);
    return p;
}

broaudio::RoutePreset toRoutePreset(const ev::Persistent& o) {
    broaudio::RoutePreset p;
    if (!ev::isObject(o.get())) return p;
    p.source  = parseModSource(objStr(o, "source", "lfo1"));
    p.dest    = parseModDest(objStr(o, "dest", "pitch"));
    p.amount  = objF(o, "amount", p.amount);
    p.enabled = objBool(o, "enabled", p.enabled);
    return p;
}

broaudio::ModPreset toModPreset(const ev::Persistent& o) {
    broaudio::ModPreset p;
    if (!ev::isObject(o.get())) return p;
    ev::Persistent lfos = objChild(o, "lfos");
    if (ev::isObject(lfos.get())) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(broaudio::ModPreset::MAX_LFOS); i++) {
            ev::Persistent e = arrayChild(lfos, i);
            if (ev::isObject(e.get())) p.lfos[i] = toLfoPreset(e);
        }
    }
    ev::Persistent routes = objChild(o, "routes");
    uint32_t n = arrayLen(routes);
    for (uint32_t i = 0; i < n; i++) {
        ev::Persistent e = arrayChild(routes, i);
        if (ev::isObject(e.get())) p.routes.push_back(toRoutePreset(e));
    }
    return p;
}

broaudio::EnginePreset toEnginePreset(const ev::Persistent& o) {
    broaudio::EnginePreset p;
    if (!ev::isObject(o.get())) return p;
    p.masterGain = objF(o, "masterGain", p.masterGain);

    ev::Persistent lim = objChild(o, "limiter");
    if (ev::isObject(lim.get())) {
        p.limiter.enabled     = objBool(lim, "enabled", p.limiter.enabled);
        p.limiter.thresholdDb = objF(lim, "thresholdDb", p.limiter.thresholdDb);
        p.limiter.releaseMs   = objF(lim, "releaseMs", p.limiter.releaseMs);
    }

    ev::Persistent mb = objChild(o, "masterBus");
    if (ev::isObject(mb.get())) p.masterBus = toBusPreset(mb);

    ev::Persistent mod = objChild(o, "modulation");
    if (ev::isObject(mod.get())) p.modulation = toModPreset(mod);

    ev::Persistent buses = objChild(o, "buses");
    uint32_t n = arrayLen(buses);
    for (uint32_t i = 0; i < n; i++) {
        ev::Persistent e = arrayChild(buses, i);
        if (ev::isObject(e.get())) p.buses.push_back(toBusPreset(e));
    }
    return p;
}

// ---- preset struct -> JS object ------------------------------------------

Value voicePresetToJs(const broaudio::VoicePreset& p) {
    ObjectBuilder o;
    o.set("waveform", waveformToString(p.waveform));
    o.set("frequency", static_cast<double>(p.frequency));
    o.set("gain", static_cast<double>(p.gain));
    o.set("pan", static_cast<double>(p.pan));
    o.set("pitchBend", static_cast<double>(p.pitchBend));
    o.set("attackTime", static_cast<double>(p.attackTime));
    o.set("decayTime", static_cast<double>(p.decayTime));
    o.set("sustainLevel", static_cast<double>(p.sustainLevel));
    o.set("releaseTime", static_cast<double>(p.releaseTime));
    o.set("filterEnabled", p.filterEnabled);
    o.set("filterType", filterTypeToString(p.filterType));
    o.set("filterFreq", static_cast<double>(p.filterFreq));
    o.set("filterQ", static_cast<double>(p.filterQ));
    o.set("unisonCount", static_cast<double>(p.unisonCount));
    o.set("unisonDetune", static_cast<double>(p.unisonDetune));
    o.set("unisonStereoWidth", static_cast<double>(p.unisonStereoWidth));
    return o.get();
}

Value filterPresetToJs(const broaudio::FilterPreset& p) {
    ObjectBuilder o;
    o.set("enabled", p.enabled);
    o.set("type", filterTypeToString(p.type));
    o.set("frequency", static_cast<double>(p.frequency));
    o.set("Q", static_cast<double>(p.Q));
    o.set("gainDB", static_cast<double>(p.gainDB));
    return o.get();
}

Value busPresetToJs(const broaudio::BusPreset& p) {
    ObjectBuilder o;
    o.set("gain", static_cast<double>(p.gain));
    o.set("pan", static_cast<double>(p.pan));

    const size_t slotCount = static_cast<size_t>(broaudio::EffectSlot::Count);
    o.set("effectOrder", hostArrayOf(slotCount, [&](size_t i) {
        return ev::fromUtf8(effectSlotToString(p.effectOrder[i]));
    }));
    o.set("filters", hostArrayOf(static_cast<size_t>(broaudio::BusPreset::MAX_FILTERS), [&](size_t i) {
        return filterPresetToJs(p.filters[i]);
    }));

    {
        ObjectBuilder d;
        d.set("enabled", p.delay.enabled);
        d.set("time", static_cast<double>(p.delay.time));
        d.set("feedback", static_cast<double>(p.delay.feedback));
        d.set("mix", static_cast<double>(p.delay.mix));
        o.set("delay", d.get());
    }
    {
        ObjectBuilder c;
        c.set("enabled", p.compressor.enabled);
        c.set("threshold", static_cast<double>(p.compressor.threshold));
        c.set("ratio", static_cast<double>(p.compressor.ratio));
        c.set("attackMs", static_cast<double>(p.compressor.attackMs));
        c.set("releaseMs", static_cast<double>(p.compressor.releaseMs));
        o.set("compressor", c.get());
    }
    {
        ObjectBuilder r;
        r.set("enabled", p.reverb.enabled);
        r.set("roomSize", static_cast<double>(p.reverb.roomSize));
        r.set("damping", static_cast<double>(p.reverb.damping));
        r.set("mix", static_cast<double>(p.reverb.mix));
        o.set("reverb", r.get());
    }
    {
        ObjectBuilder c;
        c.set("enabled", p.chorus.enabled);
        c.set("rate", static_cast<double>(p.chorus.rate));
        c.set("depth", static_cast<double>(p.chorus.depth));
        c.set("mix", static_cast<double>(p.chorus.mix));
        c.set("feedback", static_cast<double>(p.chorus.feedback));
        c.set("baseDelay", static_cast<double>(p.chorus.baseDelay));
        o.set("chorus", c.get());
    }
    {
        ObjectBuilder d;
        d.set("enabled", p.distortion.enabled);
        d.set("mode", distortionModeToString(p.distortion.mode));
        d.set("drive", static_cast<double>(p.distortion.drive));
        d.set("mix", static_cast<double>(p.distortion.mix));
        d.set("outputGain", static_cast<double>(p.distortion.outputGain));
        d.set("crushBits", static_cast<double>(p.distortion.crushBits));
        d.set("crushRate", static_cast<double>(p.distortion.crushRate));
        o.set("distortion", d.get());
    }
    {
        ObjectBuilder e;
        e.set("enabled", p.eq.enabled);
        e.set("masterGain", static_cast<double>(p.eq.masterGain));
        e.set("bandGains", hostArrayOf(7, [&](size_t i) {
            return ev::fromDouble(p.eq.bandGains[i]);
        }));
        o.set("eq", e.get());
    }
    return o.get();
}

Value modPresetToJs(const broaudio::ModPreset& p) {
    ObjectBuilder o;
    o.set("lfos", hostArrayOf(static_cast<size_t>(broaudio::ModPreset::MAX_LFOS), [&](size_t i) {
        const broaudio::LfoPreset& l = p.lfos[i];
        ObjectBuilder lo;
        lo.set("shape", lfoShapeToString(l.shape));
        lo.set("rate", static_cast<double>(l.rate));
        lo.set("depth", static_cast<double>(l.depth));
        lo.set("offset", static_cast<double>(l.offset));
        lo.set("bipolar", l.bipolar);
        lo.set("sync", l.sync);
        return lo.get();
    }));
    o.set("routes", hostArrayOf(p.routes.size(), [&](size_t i) {
        const broaudio::RoutePreset& r = p.routes[i];
        ObjectBuilder ro;
        ro.set("source", modSourceToString(r.source));
        ro.set("dest", modDestToString(r.dest));
        ro.set("amount", static_cast<double>(r.amount));
        ro.set("enabled", r.enabled);
        return ro.get();
    }));
    return o.get();
}

Value enginePresetToJs(const broaudio::EnginePreset& p) {
    ObjectBuilder o;
    o.set("masterGain", static_cast<double>(p.masterGain));
    {
        ObjectBuilder lim;
        lim.set("enabled", p.limiter.enabled);
        lim.set("thresholdDb", static_cast<double>(p.limiter.thresholdDb));
        lim.set("releaseMs", static_cast<double>(p.limiter.releaseMs));
        o.set("limiter", lim.get());
    }
    o.set("masterBus", busPresetToJs(p.masterBus));
    o.set("modulation", modPresetToJs(p.modulation));
    o.set("buses", hostArrayOf(p.buses.size(), [&](size_t i) {
        return busPresetToJs(p.buses[i]);
    }));
    return o.get();
}

// The JSON-string argument of a *PresetFromJson call: a string is taken as
// is; anything else is ToString'd the way the old binding's JS_ToCString did.
std::string jsonArg(Value v) {
    if (ev::isObject(v)) return "";
    return ev::toUtf8(v);
}

}  // namespace

void registerAudioContextPresets(ObjectBuilder& b) {
    // ---- object -> JSON ---------------------------------------------------
    b.def("voicePresetToJson", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        ev::Persistent o(a[0]);
        return ev::fromUtf8(broaudio::toJson(toVoicePreset(o)));
    });
    b.def("busPresetToJson", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        ev::Persistent o(a[0]);
        return ev::fromUtf8(broaudio::toJson(toBusPreset(o)));
    });
    b.def("modPresetToJson", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        ev::Persistent o(a[0]);
        return ev::fromUtf8(broaudio::toJson(toModPreset(o)));
    });
    b.def("enginePresetToJson", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        ev::Persistent o(a[0]);
        return ev::fromUtf8(broaudio::toJson(toEnginePreset(o)));
    });

    // ---- JSON -> object ---------------------------------------------------
    b.def("voicePresetFromJson", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        return voicePresetToJs(broaudio::voicePresetFromJson(jsonArg(a[0])));
    });
    b.def("busPresetFromJson", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        return busPresetToJs(broaudio::busPresetFromJson(jsonArg(a[0])));
    });
    b.def("modPresetFromJson", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        return modPresetToJs(broaudio::modPresetFromJson(jsonArg(a[0])));
    });
    b.def("enginePresetFromJson", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        return enginePresetToJs(broaudio::enginePresetFromJson(jsonArg(a[0])));
    });

    // ---- apply to the live engine ------------------------------------------
    b.def("applyVoicePreset", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) {
            int voiceId = i32At(a, 0);
            ev::Persistent o(a[1]);
            e->applyVoicePreset(voiceId, toVoicePreset(o));
        }
        return ev::undefined();
    });
    b.def("applyBusPreset", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) {
            int busId = i32At(a, 0);
            ev::Persistent o(a[1]);
            e->applyBusPreset(busId, toBusPreset(o));
        }
        return ev::undefined();
    });
    b.def("applyModPreset", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) {
            ev::Persistent o(a[0]);
            e->applyModPreset(toModPreset(o));
        }
        return ev::undefined();
    });
    b.def("applyEnginePreset", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) {
            ev::Persistent o(a[0]);
            e->applyEnginePreset(toEnginePreset(o));
        }
        return ev::undefined();
    });

    // ---- files (through the host's path resolver) --------------------------
    b.def("savePreset", 2, [](Value, std::span<const Value> a) {
        if (a.size() < 2) return ev::fromBool(false);
        std::string json = ev::toUtf8(a[0]);
        std::string path = resolveAudioWritePath(ev::toUtf8(a[1]));
        return ev::fromBool(broaudio::savePresetToFile(json, path.c_str()));
    });
    b.def("loadPreset", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        std::string path = resolveAudioPath(ev::toUtf8(a[0]));
        std::string s = broaudio::loadPresetFromFile(path.c_str());
        if (s.empty()) return ev::null();
        return ev::fromUtf8(s);
    });
}

}  // namespace broaudio::api
