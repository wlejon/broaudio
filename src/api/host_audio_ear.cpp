// `bro.ear`: offline analysis of a finished clip (include/broaudio/ear/ear.h)
// for scripts that judge a sound from numbers and pictures: measure(),
// compare() and spectrogram().
//
// A clip argument is any of
//   - a path string, decoded with loadAudioFile and resolved like every
//     other broaudio file API (api.h setPathResolver);
//   - an AudioBuffer;
//   - an object {samples: Float32Array, sampleRate, channels?} with the
//     samples interleaved (what decodeAudioFile and the TTS APIs return);
//   - a bare Float32Array (mono), with opts.sampleRate.
// Multichannel input is averaged to mono. Everything runs synchronously on
// the calling thread and is deterministic.
//
// `bro.ear` is shared: another library (brosoundml's CLAP scorer) mounts
// `loadClap` onto the same object, so installEar adds its three functions to
// an existing `bro.ear` instead of replacing it.

#include "host_audio_internal.h"

#include <broaudio/ear/ear.h>

#include <filesystem>

namespace broaudio::api {

namespace {

namespace ear = broaudio::ear;

struct ClipArg {
    ear::Clip clip;
    std::string label;  // a path's file name; empty otherwise
};

// A finite number property of `obj` (rooted by the caller), or false.
bool numberProp(Value obj, std::string_view name, double& out) {
    if (!ev::isObject(obj)) return false;
    Value v = ev::getProperty(obj, name);
    if (!ev::isNumber(v)) return false;
    const double d = ev::toDouble(v);
    if (!std::isfinite(d)) return false;
    out = d;
    return true;
}

bool boolProp(Value obj, std::string_view name, bool& out) {
    if (!ev::isObject(obj)) return false;
    Value v = ev::getProperty(obj, name);
    if (!ev::isBool(v)) return false;
    out = ev::toBool(v);
    return true;
}

bool stringProp(Value obj, std::string_view name, std::string& out) {
    if (!ev::isObject(obj)) return false;
    Value v = ev::getProperty(obj, name);
    if (!ev::isString(v)) return false;
    out = ev::toUtf8(v);
    return true;
}

// Reads clip argument `v` into `out`. `fallbackRate` (0 = none) is the rate
// of a bare Float32Array. A bad argument throws into the script (the embed
// throw* calls unwind and never return).
bool readClip(Value v, double fallbackRate, const char* who, ClipArg& out) {
    ev::Persistent root(v);
    if (ev::isString(root.get())) {
        const std::string given = ev::toUtf8(root.get());
        const std::string path = resolveAudioPath(given);
        std::string err;
        if (!ear::loadClip(path, out.clip, &err)) {
            ev::throwError(std::string(who) + ": " + (err.empty() ? "could not decode " + given : err));
            return false;
        }
        out.label = std::filesystem::path(given).filename().string();
        return true;
    }
    if (BufferRef buf = hostAudioBufferRef(root.get())) {
        const int chs = std::max(1, static_cast<int>(buf->channels.size()));
        std::vector<float> inter(static_cast<size_t>(buf->length) * chs, 0.0f);
        for (int c = 0; c < static_cast<int>(buf->channels.size()); ++c) {
            const auto& ch = buf->channels[c];
            for (size_t i = 0; i < ch.size() && i < static_cast<size_t>(buf->length); ++i) {
                inter[i * chs + c] = ch[i];
            }
        }
        out.clip = ear::monoClip(inter.data(), static_cast<size_t>(buf->length), chs, buf->sampleRate);
        return true;
    }
    const std::string what = std::string(who) + ": clip";
    if (ev::isTypedArray(root.get())) {
        std::vector<float> samples;
        if (!readFloatArrayArg(root.get(), FloatArrayArg::Float32Only, what.c_str(), samples)) return false;
        if (!(fallbackRate > 0)) {
            ev::throwTypeError(std::string(who) + ": a bare Float32Array clip needs opts.sampleRate");
            return false;
        }
        out.clip = ear::monoClip(samples.data(), samples.size(), 1, static_cast<int>(std::lround(fallbackRate)));
        return true;
    }
    if (ev::isObject(root.get()) && !ev::isFunction(root.get())) {
        Value sv = ev::getProperty(root.get(), "samples");
        if (!ev::isUndefined(sv)) {
            std::vector<float> samples;
            if (!readFloatArrayArg(sv, FloatArrayArg::Float32Only, (what + ".samples").c_str(), samples)) {
                return false;
            }
            double rate = fallbackRate, chans = 1;
            numberProp(root.get(), "sampleRate", rate);
            if (!numberProp(root.get(), "channels", chans)) numberProp(root.get(), "numberOfChannels", chans);
            const int ch = std::clamp(static_cast<int>(chans), 1, 64);
            if (!(rate > 0)) {
                ev::throwTypeError(std::string(who) + ": clip.sampleRate must be a positive number");
                return false;
            }
            out.clip = ear::monoClip(samples.data(), samples.size() / static_cast<size_t>(ch), ch,
                                     static_cast<int>(std::lround(rate)));
            return true;
        }
    }
    ev::throwTypeError(std::string(who) +
                       ": clip must be a path, an AudioBuffer, {samples, sampleRate, channels?} "
                       "or a Float32Array with opts.sampleRate");
    return false;
}

// The rate a bare Float32Array clip has: opts.sampleRate, else none.
double optRate(std::span<const Value> a, size_t optIndex) {
    double r = 0;
    if (optIndex < a.size()) numberProp(a[optIndex], "sampleRate", r);
    return r;
}

void setNum(ObjectBuilder& b, std::string_view name, double d) {
    if (std::isnan(d)) b.set(name, ev::null());
    else b.set(name, ev::fromDouble(d));
}

Value measurementValue(const ear::Measurement& m) {
    ObjectBuilder r;
    r.set("sampleRate", static_cast<double>(m.sampleRate));
    r.set("channels", static_cast<double>(m.channels));
    r.set("duration", m.duration);
    r.set("peakTime", m.peakTime);
    r.set("envelopePeakTime", m.envelopePeakTime);
    r.set("onsetTime", m.onsetTime);
    r.set("attackTime", m.attackTime);
    r.set("tailTime", m.tailTime);
    r.set("tailEnd", m.tailEnd);
    setNum(r, "noiseFloorDb", m.noiseFloorDb);
    r.set("decayRate", m.decayRate);
    setNum(r, "t60", m.t60);
    r.set("peakDb", m.peakDb);
    r.set("envelopePeakDb", m.envelopePeakDb);
    r.set("rmsDb", m.rmsDb);
    setNum(r, "lufs", m.lufs);
    r.set("centroidHz", m.centroidHz);
    r.set("flatness", m.flatness);
    r.set("tonality", m.tonality);
    {
        ev::Persistent arr(hostArrayOf(m.timeline.size(), [&](size_t i) {
            const ear::Slice& s = m.timeline[i];
            ObjectBuilder o;
            o.set("time", s.time);
            o.set("duration", s.duration);
            o.set("rmsDb", s.rmsDb);
            o.set("centroidHz", s.centroidHz);
            o.set("flatness", s.flatness);
            o.set("tonality", s.tonality);
            return o.get();
        }));
        r.set("timeline", arr.get());
    }
    {
        ev::Persistent arr(hostArrayOf(m.partials.size(), [&](size_t i) {
            const ear::Partial& p = m.partials[i];
            ObjectBuilder o;
            o.set("freqHz", p.freqHz);
            o.set("peakDb", p.peakDb);
            o.set("startTime", p.startTime);
            o.set("peakTime", p.peakTime);
            o.set("endTime", p.endTime);
            o.set("ringTime", p.ringTime);
            o.set("decayRate", p.decayRate);
            setNum(o, "t60", p.t60);
            o.set("stabilityCents", p.stabilityCents);
            o.set("energyShare", p.energyShare);
            setNum(o, "ratio", p.ratio);
            return o.get();
        }));
        r.set("partials", arr.get());
    }
    {
        const ear::Ringing& g = m.ringing;
        ObjectBuilder o;
        o.set("count", static_cast<double>(g.count));
        o.set("sparsity", g.sparsity);
        o.set("strongestRingTime", g.strongestRingTime);
        o.set("weightedRingTime", g.weightedRingTime);
        setNum(o, "f0Hz", g.f0Hz);
        o.set("inharmonicity", g.inharmonicity);
        o.set("ringScore", g.ringScore);
        r.set("ringing", o.get());
    }
    return r.get();
}

Value measureFn(Value, std::span<const Value> a) {
    if (a.empty()) return ev::throwTypeError("bro.ear.measure(clip, opts?): clip is required");
    ClipArg c;
    if (!readClip(a[0], optRate(a, 1), "bro.ear.measure", c)) return ev::undefined();
    ear::MeasureOptions o;
    if (a.size() > 1 && ev::isObject(a[1])) {
        double d;
        if (numberProp(a[1], "tailFloorDb", d)) o.tailFloorDb = static_cast<float>(std::clamp(d, -200.0, -1.0));
        if (numberProp(a[1], "maxPartials", d)) o.maxPartials = static_cast<int>(std::clamp(d, 0.0, 256.0));
        if (numberProp(a[1], "slices", d)) o.slices = static_cast<int>(std::clamp(d, 1.0, 1024.0));
    }
    return measurementValue(ear::measure(c.clip, o));
}

Value compareFn(Value, std::span<const Value> a) {
    if (a.size() < 2) return ev::throwTypeError("bro.ear.compare(clip, reference, opts?): two clips are required");
    const double rate = optRate(a, 2);
    ClipArg c, ref;
    if (!readClip(a[0], rate, "bro.ear.compare", c)) return ev::undefined();
    if (!readClip(a[1], rate, "bro.ear.compare", ref)) return ev::undefined();
    ear::CompareOptions o;
    if (a.size() > 2 && ev::isObject(a[2])) {
        double d;
        bool b;
        if (boolProp(a[2], "align", b)) o.align = b;
        if (numberProp(a[2], "maxShift", d)) o.maxShift = std::clamp(d, 0.0, 10.0);
        Value wv = ev::getProperty(a[2], "weights");
        if (ev::isObject(wv)) {
            ev::Persistent w(wv);
            if (numberProp(w.get(), "envelope", d)) o.envelopeWeight = d;
            if (numberProp(w.get(), "spectrum", d)) o.spectrumWeight = d;
            if (numberProp(w.get(), "tonality", d)) o.tonalityWeight = d;
        }
    }
    const ear::Comparison r = ear::compare(c.clip, ref.clip, o);
    ObjectBuilder out;
    setNum(out, "score", r.score);
    setNum(out, "envelope", r.envelope);
    setNum(out, "spectrum", r.spectrum);
    setNum(out, "tonality", r.tonality);
    out.set("sampleRate", static_cast<double>(r.sampleRate));
    out.set("offsetTime", r.offsetTime);
    out.set("loudnessDiffDb", r.loudnessDiffDb);
    out.set("envelopeDb", r.envelopeDb);
    out.set("spectrogramDb", r.spectrogramDb);
    out.set("ltasDb", r.ltasDb);
    out.set("centroidRatio", r.centroidRatio);
    out.set("tonalityDiff", r.tonalityDiff);
    out.set("ringTimeRatio", r.ringTimeRatio);
    out.set("inharmonicityDiff", r.inharmonicityDiff);
    out.set("durationDiff", r.durationDiff);
    return out.get();
}

// A single clip, or an array-like of clips?
bool isClipList(Value v) {
    if (!ev::isObject(v) || ev::isTypedArray(v) || ev::isFunction(v)) return false;
    if (hostAudioBufferRef(v)) return false;
    ev::Persistent root(v);
    if (!ev::isUndefined(ev::getProperty(root.get(), "samples"))) return false;
    return ev::isNumber(ev::getProperty(root.get(), "length"));
}

// bro.image.encodePngFile when the host has it (a real deflate), else the
// ear's own stored-block writer. True when the file was written.
bool writeSpectrogramPng(const std::string& path, Value data, int w, int h, const ear::SpectrogramImage& img) {
    ev::Persistent dataRoot(data);
    ev::GlobalValue broV = ev::globalValue("bro");
    if (broV.found && ev::isObject(broV.value)) {
        Value imageV = ev::getProperty(broV.value, "image");
        if (ev::isObject(imageV)) {
            ev::Persistent image(imageV);
            Value fnV = ev::getProperty(image.get(), "encodePngFile");
            if (ev::isFunction(fnV)) {
                ev::Persistent fn(fnV);
                ev::Persistent pathV(ev::fromUtf8(path));
                const Value args[5] = {pathV.get(), dataRoot.get(), ev::fromDouble(w), ev::fromDouble(h),
                                       ev::fromDouble(4)};
                ev::CallResult r = ev::call(fn.get(), image.get(), std::span<const Value>(args, 5));
                if (r.thrown) {
                    ev::throwValue(r.value);
                    return false;
                }
                return ev::toBool(r.value);
            }
        }
    }
    return ear::writePng(resolveAudioWritePath(path), img.rgba.data(), img.width, img.height);
}

Value spectrogramFn(Value, std::span<const Value> a) {
    if (a.empty()) return ev::throwTypeError("bro.ear.spectrogram(clipOrClips, opts?): a clip is required");
    const char* who = "bro.ear.spectrogram";
    const double rate = optRate(a, 1);
    std::vector<ClipArg> clips;
    if (isClipList(a[0])) {
        ev::Persistent list(a[0]);
        const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(list.get(), "length")));
        if (n == 0 || n > 64) return ev::throwRangeError(std::string(who) + ": between 1 and 64 clips");
        clips.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            Value e = ev::getElement(list.get(), i);
            if (!readClip(e, rate, who, clips[i])) return ev::undefined();
        }
    } else {
        clips.resize(1);
        if (!readClip(a[0], rate, who, clips[0])) return ev::undefined();
    }

    ear::SpectrogramOptions o;
    std::vector<std::string> labels;
    std::string path;
    for (const ClipArg& c : clips) labels.push_back(c.label);
    if (a.size() > 1 && ev::isObject(a[1])) {
        const Value& opts = a[1];
        double d;
        std::string s;
        if (numberProp(opts, "width", d)) o.width = static_cast<int>(d);
        if (numberProp(opts, "height", d)) o.height = static_cast<int>(d);
        if (numberProp(opts, "minHz", d)) o.minHz = d;
        if (numberProp(opts, "maxHz", d)) o.maxHz = d;
        if (numberProp(opts, "dbRange", d)) o.dbRange = d;
        if (numberProp(opts, "maxDb", d)) o.maxDb = d;
        if (numberProp(opts, "fftSize", d)) o.fftSize = static_cast<int>(std::clamp(d, 0.0, 32768.0));
        if (numberProp(opts, "fontScale", d)) o.fontScale = static_cast<int>(d);
        if (stringProp(opts, "layout", s)) {
            if (s == "stack") o.stacked = true;
            else if (s == "side") o.stacked = false;
            else return ev::throwTypeError(std::string(who) + ": layout must be 'stack' or 'side'");
        }
        if (stringProp(opts, "scale", s)) {
            if (s == "log") o.scale = ear::FrequencyScale::Log;
            else if (s == "mel") o.scale = ear::FrequencyScale::Mel;
            else if (s == "linear") o.scale = ear::FrequencyScale::Linear;
            else return ev::throwTypeError(std::string(who) + ": scale must be 'log', 'mel' or 'linear'");
        }
        stringProp(opts, "path", path);
        Value lv = ev::getProperty(opts, "labels");
        if (ev::isObject(lv)) {
            ev::Persistent lr(lv);
            const uint32_t n = saturateU32(ev::toDouble(ev::getProperty(lr.get(), "length")));
            for (uint32_t i = 0; i < n && i < labels.size(); ++i) {
                Value e = ev::getElement(lr.get(), i);
                if (ev::isString(e)) labels[i] = ev::toUtf8(e);
            }
        }
    }

    std::vector<const ear::Clip*> ptrs;
    for (const ClipArg& c : clips) ptrs.push_back(&c.clip);
    const ear::SpectrogramImage img = ear::spectrogram(ptrs, labels, o);
    if (img.rgba.empty()) return ev::throwError(std::string(who) + ": nothing to draw (no clip has a sample rate)");

    ev::Persistent data(ev::createTypedArray(ev::elements::Uint8Clamped, static_cast<uint32_t>(img.rgba.size())));
    ev::fillTypedArray(data.get(), std::span<const uint8_t>(img.rgba.data(), img.rgba.size()));
    if (!path.empty()) {
        if (!writeSpectrogramPng(path, data.get(), img.width, img.height, img)) {
            return ev::throwError(std::string(who) + ": could not write " + path);
        }
    }
    ObjectBuilder out;
    out.set("width", static_cast<double>(img.width));
    out.set("height", static_cast<double>(img.height));
    out.set("data", data.get());
    out.set("duration", img.duration);
    out.set("minHz", img.minHz);
    out.set("maxHz", img.maxHz);
    out.set("minDb", img.minDb);
    out.set("maxDb", img.maxDb);
    {
        ev::Persistent arr(hostArrayOf(img.panels.size(), [&](size_t i) {
            const ear::SpectrogramPanel& p = img.panels[i];
            ObjectBuilder po;
            po.set("label", p.label);
            po.set("x", static_cast<double>(p.x));
            po.set("y", static_cast<double>(p.y));
            po.set("width", static_cast<double>(p.width));
            po.set("height", static_cast<double>(p.height));
            po.set("duration", p.duration);
            po.set("sampleRate", static_cast<double>(p.sampleRate));
            return po.get();
        }));
        out.set("panels", arr.get());
    }
    if (!path.empty()) out.set("path", path);
    return out.get();
}

} // namespace

void installEar() {
    Value broVal = ev::getGlobal("bro");
    ObjectBuilder broObj = ev::isObject(broVal) ? ObjectBuilder(broVal) : ObjectBuilder();
    Value earVal = ev::getProperty(broObj.get(), "ear");
    ObjectBuilder earObj = ev::isObject(earVal) ? ObjectBuilder(earVal) : ObjectBuilder();
    earObj.def("measure", 2, measureFn);
    earObj.def("compare", 3, compareFn);
    earObj.def("spectrogram", 2, spectrogramFn);
    broObj.set("ear", earObj.get());
    ev::setGlobalValue("bro", broObj.get());
}

} // namespace broaudio::api
