#include "broaudio/io/serialization.h"

#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace broaudio {

// ---------------------------------------------------------------------------
// Enum string tables
// ---------------------------------------------------------------------------

static const char* waveformName(Waveform w) {
    switch (w) {
        case Waveform::Sine:       return "sine";
        case Waveform::Square:     return "square";
        case Waveform::Sawtooth:   return "sawtooth";
        case Waveform::Triangle:   return "triangle";
        case Waveform::Wavetable:  return "wavetable";
        case Waveform::WhiteNoise: return "white_noise";
        case Waveform::PinkNoise:  return "pink_noise";
        case Waveform::BrownNoise: return "brown_noise";
    }
    return "sine";
}

static Waveform waveformFromName(const std::string& s) {
    if (s == "square")     return Waveform::Square;
    if (s == "sawtooth")   return Waveform::Sawtooth;
    if (s == "triangle")   return Waveform::Triangle;
    if (s == "wavetable")  return Waveform::Wavetable;
    if (s == "white_noise") return Waveform::WhiteNoise;
    if (s == "pink_noise")  return Waveform::PinkNoise;
    if (s == "brown_noise") return Waveform::BrownNoise;
    return Waveform::Sine;
}

static const char* filterTypeName(BiquadFilter::Type t) {
    switch (t) {
        case BiquadFilter::Type::Lowpass:   return "lowpass";
        case BiquadFilter::Type::Highpass:  return "highpass";
        case BiquadFilter::Type::Bandpass:  return "bandpass";
        case BiquadFilter::Type::Notch:     return "notch";
        case BiquadFilter::Type::Allpass:   return "allpass";
        case BiquadFilter::Type::Peaking:   return "peaking";
        case BiquadFilter::Type::Lowshelf:  return "low_shelf";
        case BiquadFilter::Type::Highshelf: return "high_shelf";
    }
    return "lowpass";
}

static BiquadFilter::Type filterTypeFromName(const std::string& s) {
    if (s == "highpass")   return BiquadFilter::Type::Highpass;
    if (s == "bandpass")   return BiquadFilter::Type::Bandpass;
    if (s == "notch")      return BiquadFilter::Type::Notch;
    if (s == "allpass")    return BiquadFilter::Type::Allpass;
    if (s == "peaking")    return BiquadFilter::Type::Peaking;
    if (s == "low_shelf")  return BiquadFilter::Type::Lowshelf;
    if (s == "high_shelf") return BiquadFilter::Type::Highshelf;
    return BiquadFilter::Type::Lowpass;
}

static const char* distortionModeName(DistortionMode m) {
    switch (m) {
        case DistortionMode::SoftClip: return "soft_clip";
        case DistortionMode::HardClip: return "hard_clip";
        case DistortionMode::Foldback: return "foldback";
        case DistortionMode::Bitcrush: return "bitcrush";
    }
    return "soft_clip";
}

static DistortionMode distortionModeFromName(const std::string& s) {
    if (s == "hard_clip") return DistortionMode::HardClip;
    if (s == "foldback")  return DistortionMode::Foldback;
    if (s == "bitcrush")  return DistortionMode::Bitcrush;
    return DistortionMode::SoftClip;
}

static const char* lfoShapeName(LfoShape s) {
    switch (s) {
        case LfoShape::Sine:          return "sine";
        case LfoShape::Triangle:      return "triangle";
        case LfoShape::Square:        return "square";
        case LfoShape::SawUp:         return "saw_up";
        case LfoShape::SawDown:       return "saw_down";
        case LfoShape::SampleAndHold: return "sample_and_hold";
    }
    return "sine";
}

static LfoShape lfoShapeFromName(const std::string& s) {
    if (s == "triangle")        return LfoShape::Triangle;
    if (s == "square")          return LfoShape::Square;
    if (s == "saw_up")          return LfoShape::SawUp;
    if (s == "saw_down")        return LfoShape::SawDown;
    if (s == "sample_and_hold") return LfoShape::SampleAndHold;
    return LfoShape::Sine;
}

static const char* modSourceName(ModSource s) {
    switch (s) {
        case ModSource::Lfo1:        return "lfo1";
        case ModSource::Lfo2:        return "lfo2";
        case ModSource::Lfo3:        return "lfo3";
        case ModSource::Lfo4:        return "lfo4";
        case ModSource::Envelope:    return "envelope";
        case ModSource::Velocity:    return "velocity";
        case ModSource::KeyTracking: return "key_tracking";
        case ModSource::ModWheel:    return "mod_wheel";
        case ModSource::Aftertouch:  return "aftertouch";
        default: return "lfo1";
    }
}

static ModSource modSourceFromName(const std::string& s) {
    if (s == "lfo1")         return ModSource::Lfo1;
    if (s == "lfo2")         return ModSource::Lfo2;
    if (s == "lfo3")         return ModSource::Lfo3;
    if (s == "lfo4")         return ModSource::Lfo4;
    if (s == "envelope")     return ModSource::Envelope;
    if (s == "velocity")     return ModSource::Velocity;
    if (s == "key_tracking") return ModSource::KeyTracking;
    if (s == "mod_wheel")    return ModSource::ModWheel;
    if (s == "aftertouch")   return ModSource::Aftertouch;
    return ModSource::Lfo1;
}

static const char* modDestName(ModDest d) {
    switch (d) {
        case ModDest::Pitch:       return "pitch";
        case ModDest::Gain:        return "gain";
        case ModDest::Pan:         return "pan";
        case ModDest::FilterFreq:  return "filter_freq";
        case ModDest::FilterQ:     return "filter_q";
        case ModDest::PulseWidth:  return "pulse_width";
        case ModDest::DelaySend:   return "delay_send";
        default: return "pitch";
    }
}

static ModDest modDestFromName(const std::string& s) {
    if (s == "pitch")       return ModDest::Pitch;
    if (s == "gain")        return ModDest::Gain;
    if (s == "pan")         return ModDest::Pan;
    if (s == "filter_freq") return ModDest::FilterFreq;
    if (s == "filter_q")    return ModDest::FilterQ;
    if (s == "pulse_width") return ModDest::PulseWidth;
    if (s == "delay_send")  return ModDest::DelaySend;
    return ModDest::Pitch;
}

static const char* effectSlotName(EffectSlot e) {
    switch (e) {
        case EffectSlot::Filter:     return "filter";
        case EffectSlot::Delay:      return "delay";
        case EffectSlot::Compressor: return "compressor";
        case EffectSlot::Chorus:     return "chorus";
        case EffectSlot::Reverb:     return "reverb";
        case EffectSlot::Equalizer:  return "equalizer";
        case EffectSlot::Distortion: return "distortion";
        default: return "filter";
    }
}

static EffectSlot effectSlotFromName(const std::string& s) {
    if (s == "filter")     return EffectSlot::Filter;
    if (s == "delay")      return EffectSlot::Delay;
    if (s == "compressor") return EffectSlot::Compressor;
    if (s == "chorus")     return EffectSlot::Chorus;
    if (s == "reverb")     return EffectSlot::Reverb;
    if (s == "equalizer")  return EffectSlot::Equalizer;
    if (s == "distortion") return EffectSlot::Distortion;
    return EffectSlot::Filter;
}

// ---------------------------------------------------------------------------
// JSON helpers — to JSON
// ---------------------------------------------------------------------------

static json voiceToJson(const VoicePreset& p) {
    json j;
    j["waveform"] = waveformName(p.waveform);
    j["frequency"] = p.frequency;
    j["gain"] = p.gain;
    j["pan"] = p.pan;
    j["pitchBend"] = p.pitchBend;

    j["adsr"] = {
        {"attack", p.attackTime},
        {"decay", p.decayTime},
        {"sustain", p.sustainLevel},
        {"release", p.releaseTime}
    };

    j["filter"] = {
        {"enabled", p.filterEnabled},
        {"type", filterTypeName(p.filterType)},
        {"frequency", p.filterFreq},
        {"q", p.filterQ}
    };

    j["unison"] = {
        {"count", p.unisonCount},
        {"detune", p.unisonDetune},
        {"stereoWidth", p.unisonStereoWidth}
    };

    return j;
}

static json filterToJson(const FilterPreset& f) {
    return {
        {"enabled", f.enabled},
        {"type", filterTypeName(f.type)},
        {"frequency", f.frequency},
        {"q", f.Q},
        {"gainDB", f.gainDB}
    };
}

static json busToJson(const BusPreset& b) {
    json j;
    j["gain"] = b.gain;
    j["pan"] = b.pan;

    // Effect order
    int numSlots = static_cast<int>(EffectSlot::Count);
    json order = json::array();
    for (int i = 0; i < numSlots; i++)
        order.push_back(effectSlotName(b.effectOrder[i]));
    j["effectOrder"] = order;

    // Filters
    json filters = json::array();
    for (int i = 0; i < BusPreset::MAX_FILTERS; i++)
        filters.push_back(filterToJson(b.filters[i]));
    j["filters"] = filters;

    j["delay"] = {
        {"enabled", b.delay.enabled},
        {"time", b.delay.time},
        {"feedback", b.delay.feedback},
        {"mix", b.delay.mix}
    };

    j["compressor"] = {
        {"enabled", b.compressor.enabled},
        {"threshold", b.compressor.threshold},
        {"ratio", b.compressor.ratio},
        {"attackMs", b.compressor.attackMs},
        {"releaseMs", b.compressor.releaseMs}
    };

    j["reverb"] = {
        {"enabled", b.reverb.enabled},
        {"roomSize", b.reverb.roomSize},
        {"damping", b.reverb.damping},
        {"mix", b.reverb.mix}
    };

    j["chorus"] = {
        {"enabled", b.chorus.enabled},
        {"rate", b.chorus.rate},
        {"depth", b.chorus.depth},
        {"mix", b.chorus.mix},
        {"feedback", b.chorus.feedback},
        {"baseDelay", b.chorus.baseDelay}
    };

    j["distortion"] = {
        {"enabled", b.distortion.enabled},
        {"mode", distortionModeName(b.distortion.mode)},
        {"drive", b.distortion.drive},
        {"mix", b.distortion.mix},
        {"outputGain", b.distortion.outputGain},
        {"crushBits", b.distortion.crushBits},
        {"crushRate", b.distortion.crushRate}
    };

    j["eq"] = {
        {"enabled", b.eq.enabled},
        {"masterGain", b.eq.masterGain},
        {"bandGains", json::array({b.eq.bandGains[0], b.eq.bandGains[1], b.eq.bandGains[2],
                                   b.eq.bandGains[3], b.eq.bandGains[4], b.eq.bandGains[5],
                                   b.eq.bandGains[6]})}
    };

    return j;
}

static json lfoToJson(const LfoPreset& l) {
    return {
        {"shape", lfoShapeName(l.shape)},
        {"rate", l.rate},
        {"depth", l.depth},
        {"offset", l.offset},
        {"bipolar", l.bipolar},
        {"sync", l.sync}
    };
}

static json routeToJson(const RoutePreset& r) {
    return {
        {"source", modSourceName(r.source)},
        {"dest", modDestName(r.dest)},
        {"amount", r.amount},
        {"enabled", r.enabled}
    };
}

static json modToJson(const ModPreset& m) {
    json j;
    json lfos = json::array();
    for (int i = 0; i < ModPreset::MAX_LFOS; i++)
        lfos.push_back(lfoToJson(m.lfos[i]));
    j["lfos"] = lfos;

    json routes = json::array();
    for (auto& r : m.routes)
        routes.push_back(routeToJson(r));
    j["routes"] = routes;

    return j;
}

// ---------------------------------------------------------------------------
// JSON helpers — from JSON
// ---------------------------------------------------------------------------

static VoicePreset voiceFromJson(const json& j) {
    VoicePreset p;
    if (j.contains("waveform"))  p.waveform = waveformFromName(j["waveform"].get<std::string>());
    if (j.contains("frequency")) p.frequency = j["frequency"].get<float>();
    if (j.contains("gain"))      p.gain = j["gain"].get<float>();
    if (j.contains("pan"))       p.pan = j["pan"].get<float>();
    if (j.contains("pitchBend")) p.pitchBend = j["pitchBend"].get<float>();

    if (j.contains("adsr")) {
        auto& a = j["adsr"];
        if (a.contains("attack"))  p.attackTime = a["attack"].get<float>();
        if (a.contains("decay"))   p.decayTime = a["decay"].get<float>();
        if (a.contains("sustain")) p.sustainLevel = a["sustain"].get<float>();
        if (a.contains("release")) p.releaseTime = a["release"].get<float>();
    }

    if (j.contains("filter")) {
        auto& f = j["filter"];
        if (f.contains("enabled"))   p.filterEnabled = f["enabled"].get<bool>();
        if (f.contains("type"))      p.filterType = filterTypeFromName(f["type"].get<std::string>());
        if (f.contains("frequency")) p.filterFreq = f["frequency"].get<float>();
        if (f.contains("q"))         p.filterQ = f["q"].get<float>();
    }

    if (j.contains("unison")) {
        auto& u = j["unison"];
        if (u.contains("count"))       p.unisonCount = u["count"].get<int>();
        if (u.contains("detune"))      p.unisonDetune = u["detune"].get<float>();
        if (u.contains("stereoWidth")) p.unisonStereoWidth = u["stereoWidth"].get<float>();
    }

    return p;
}

static FilterPreset filterFromJson(const json& j) {
    FilterPreset f;
    if (j.contains("enabled"))   f.enabled = j["enabled"].get<bool>();
    if (j.contains("type"))      f.type = filterTypeFromName(j["type"].get<std::string>());
    if (j.contains("frequency")) f.frequency = j["frequency"].get<float>();
    if (j.contains("q"))         f.Q = j["q"].get<float>();
    if (j.contains("gainDB"))    f.gainDB = j["gainDB"].get<float>();
    return f;
}

static BusPreset busFromJson(const json& j) {
    BusPreset b;
    if (j.contains("gain")) b.gain = j["gain"].get<float>();
    if (j.contains("pan"))  b.pan = j["pan"].get<float>();

    if (j.contains("effectOrder")) {
        auto& arr = j["effectOrder"];
        int count = static_cast<int>(EffectSlot::Count);
        for (int i = 0; i < count && i < static_cast<int>(arr.size()); i++)
            b.effectOrder[i] = effectSlotFromName(arr[i].get<std::string>());
    }

    if (j.contains("filters")) {
        auto& arr = j["filters"];
        for (int i = 0; i < BusPreset::MAX_FILTERS && i < static_cast<int>(arr.size()); i++)
            b.filters[i] = filterFromJson(arr[i]);
    }

    if (j.contains("delay")) {
        auto& d = j["delay"];
        if (d.contains("enabled"))  b.delay.enabled = d["enabled"].get<bool>();
        if (d.contains("time"))     b.delay.time = d["time"].get<float>();
        if (d.contains("feedback")) b.delay.feedback = d["feedback"].get<float>();
        if (d.contains("mix"))      b.delay.mix = d["mix"].get<float>();
    }

    if (j.contains("compressor")) {
        auto& c = j["compressor"];
        if (c.contains("enabled"))   b.compressor.enabled = c["enabled"].get<bool>();
        if (c.contains("threshold")) b.compressor.threshold = c["threshold"].get<float>();
        if (c.contains("ratio"))     b.compressor.ratio = c["ratio"].get<float>();
        if (c.contains("attackMs"))  b.compressor.attackMs = c["attackMs"].get<float>();
        if (c.contains("releaseMs")) b.compressor.releaseMs = c["releaseMs"].get<float>();
    }

    if (j.contains("reverb")) {
        auto& r = j["reverb"];
        if (r.contains("enabled"))  b.reverb.enabled = r["enabled"].get<bool>();
        if (r.contains("roomSize")) b.reverb.roomSize = r["roomSize"].get<float>();
        if (r.contains("damping"))  b.reverb.damping = r["damping"].get<float>();
        if (r.contains("mix"))      b.reverb.mix = r["mix"].get<float>();
    }

    if (j.contains("chorus")) {
        auto& c = j["chorus"];
        if (c.contains("enabled"))   b.chorus.enabled = c["enabled"].get<bool>();
        if (c.contains("rate"))      b.chorus.rate = c["rate"].get<float>();
        if (c.contains("depth"))     b.chorus.depth = c["depth"].get<float>();
        if (c.contains("mix"))       b.chorus.mix = c["mix"].get<float>();
        if (c.contains("feedback"))  b.chorus.feedback = c["feedback"].get<float>();
        if (c.contains("baseDelay")) b.chorus.baseDelay = c["baseDelay"].get<float>();
    }

    if (j.contains("distortion")) {
        auto& d = j["distortion"];
        if (d.contains("enabled"))    b.distortion.enabled = d["enabled"].get<bool>();
        if (d.contains("mode"))       b.distortion.mode = distortionModeFromName(d["mode"].get<std::string>());
        if (d.contains("drive"))      b.distortion.drive = d["drive"].get<float>();
        if (d.contains("mix"))        b.distortion.mix = d["mix"].get<float>();
        if (d.contains("outputGain")) b.distortion.outputGain = d["outputGain"].get<float>();
        if (d.contains("crushBits"))  b.distortion.crushBits = d["crushBits"].get<float>();
        if (d.contains("crushRate"))  b.distortion.crushRate = d["crushRate"].get<float>();
    }

    if (j.contains("eq")) {
        auto& e = j["eq"];
        if (e.contains("enabled"))    b.eq.enabled = e["enabled"].get<bool>();
        if (e.contains("masterGain")) b.eq.masterGain = e["masterGain"].get<float>();
        if (e.contains("bandGains")) {
            auto& arr = e["bandGains"];
            for (int i = 0; i < 7 && i < static_cast<int>(arr.size()); i++)
                b.eq.bandGains[i] = arr[i].get<float>();
        }
    }

    return b;
}

static LfoPreset lfoFromJson(const json& j) {
    LfoPreset l;
    if (j.contains("shape"))   l.shape = lfoShapeFromName(j["shape"].get<std::string>());
    if (j.contains("rate"))    l.rate = j["rate"].get<float>();
    if (j.contains("depth"))   l.depth = j["depth"].get<float>();
    if (j.contains("offset"))  l.offset = j["offset"].get<float>();
    if (j.contains("bipolar")) l.bipolar = j["bipolar"].get<bool>();
    if (j.contains("sync"))    l.sync = j["sync"].get<bool>();
    return l;
}

static RoutePreset routeFromJson(const json& j) {
    RoutePreset r;
    if (j.contains("source"))  r.source = modSourceFromName(j["source"].get<std::string>());
    if (j.contains("dest"))    r.dest = modDestFromName(j["dest"].get<std::string>());
    if (j.contains("amount"))  r.amount = j["amount"].get<float>();
    if (j.contains("enabled")) r.enabled = j["enabled"].get<bool>();
    return r;
}

static ModPreset modFromJson(const json& j) {
    ModPreset m;
    if (j.contains("lfos")) {
        auto& arr = j["lfos"];
        for (int i = 0; i < ModPreset::MAX_LFOS && i < static_cast<int>(arr.size()); i++)
            m.lfos[i] = lfoFromJson(arr[i]);
    }
    if (j.contains("routes")) {
        for (auto& r : j["routes"])
            m.routes.push_back(routeFromJson(r));
    }
    return m;
}

// ---------------------------------------------------------------------------
// Public API — toJson
// ---------------------------------------------------------------------------

std::string toJson(const VoicePreset& preset)
{
    return voiceToJson(preset).dump(2);
}

std::string toJson(const BusPreset& preset)
{
    return busToJson(preset).dump(2);
}

std::string toJson(const ModPreset& preset)
{
    return modToJson(preset).dump(2);
}

std::string toJson(const EnginePreset& preset)
{
    json j;
    j["masterGain"] = preset.masterGain;

    j["limiter"] = {
        {"enabled", preset.limiter.enabled},
        {"thresholdDb", preset.limiter.thresholdDb},
        {"releaseMs", preset.limiter.releaseMs}
    };

    j["masterBus"] = busToJson(preset.masterBus);

    json buses = json::array();
    for (auto& b : preset.buses)
        buses.push_back(busToJson(b));
    j["buses"] = buses;

    j["modulation"] = modToJson(preset.modulation);

    return j.dump(2);
}

// ---------------------------------------------------------------------------
// Public API — fromJson
// ---------------------------------------------------------------------------

VoicePreset voicePresetFromJson(const std::string& str)
{
    return voiceFromJson(json::parse(str));
}

BusPreset busPresetFromJson(const std::string& str)
{
    return busFromJson(json::parse(str));
}

ModPreset modPresetFromJson(const std::string& str)
{
    return modFromJson(json::parse(str));
}

EnginePreset enginePresetFromJson(const std::string& str)
{
    json j = json::parse(str);
    EnginePreset p;

    if (j.contains("masterGain")) p.masterGain = j["masterGain"].get<float>();

    if (j.contains("limiter")) {
        auto& l = j["limiter"];
        if (l.contains("enabled"))     p.limiter.enabled = l["enabled"].get<bool>();
        if (l.contains("thresholdDb")) p.limiter.thresholdDb = l["thresholdDb"].get<float>();
        if (l.contains("releaseMs"))   p.limiter.releaseMs = l["releaseMs"].get<float>();
    }

    if (j.contains("masterBus"))
        p.masterBus = busFromJson(j["masterBus"]);

    if (j.contains("buses")) {
        for (auto& b : j["buses"])
            p.buses.push_back(busFromJson(b));
    }

    if (j.contains("modulation"))
        p.modulation = modFromJson(j["modulation"]);

    return p;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

bool savePresetToFile(const std::string& jsonStr, const char* path)
{
    if (!path) return false;
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << jsonStr;
    return file.good();
}

std::string loadPresetFromFile(const char* path)
{
    if (!path) return {};
    std::ifstream file(path);
    if (!file.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

} // namespace broaudio
