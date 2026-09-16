// Standalone test for broaudio_api, the bronze-runtime JavaScript binding.
// No bro engine, no window: a fresh bronze realm, installAudio() (which
// lazily creates a broaudio::Engine, falling back to headless when no output
// device opens), then the Web Audio mount points and a node graph checked
// from both the embed API and a compiled script. Nothing here calls
// resume() or starts real playback.

#include "api.h"
#include "embed/embed.h"
#include "eval/eval.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace ev = bronze::embed;
using Value = bronze::Value;

#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::exit(1); \
    } \
} while (0)

static std::string errorName(Value thrown) {
    return ev::isObject(thrown) ? ev::toUtf8(ev::getProperty(thrown, "name")) : std::string();
}

static void test_mounts() {
    std::cout << "[1/3] mount points..." << std::endl;

    const char* ctors[] = {
        "AudioContext", "webkitAudioContext", "AudioNode", "AudioParam",
        "GainNode", "OscillatorNode", "AudioBuffer", "AudioBufferSourceNode",
        "BiquadFilterNode", "AnalyserNode", "PannerNode", "StereoPannerNode",
        "DelayNode", "DynamicsCompressorNode", "WaveShaperNode", "ConvolverNode",
        "ChannelSplitterNode", "ChannelMergerNode",
    };
    for (const char* name : ctors) {
        ev::GlobalValue c = ev::globalValue(name);
        if (!c.found || !ev::isFunction(c.value)) {
            std::cerr << "missing global constructor " << name << std::endl;
            std::exit(1);
        }
    }
}

static void test_context_direct() {
    std::cout << "[2/3] AudioContext via the embed API..." << std::endl;

    Value ctor = ev::globalValue("AudioContext").value;
    ev::CallResult made = ev::construct(ctor, {});
    TEST_CHECK(!made.thrown);
    TEST_CHECK(ev::isObject(made.value));
    ev::Persistent ctx(made.value);

    // Doc'd readonly properties.
    Value sr = ev::getProperty(ctx.get(), "sampleRate");
    TEST_CHECK(ev::isNumber(sr));
    TEST_CHECK(ev::toDouble(sr) > 0.0);
    Value state = ev::getProperty(ctx.get(), "state");
    TEST_CHECK(ev::isString(state));
    Value ct = ev::getProperty(ctx.get(), "currentTime");
    TEST_CHECK(ev::isNumber(ct));
    Value dest = ev::getProperty(ctx.get(), "destination");
    TEST_CHECK(ev::isObject(dest));

    // Node factories return objects carrying the doc'd members.
    const char* factories[] = {
        "createOscillator", "createGain", "createBiquadFilter", "createAnalyser",
        "createVoiceAllocator", "createModMatrix", "createSequence",
    };
    for (const char* name : factories) {
        Value fn = ev::getProperty(ctx.get(), name);
        if (!ev::isFunction(fn)) {
            std::cerr << "missing AudioContext." << name << std::endl;
            std::exit(1);
        }
    }

    Value createGain = ev::getProperty(ctx.get(), "createGain");
    ev::CallResult gainRes = ev::call(createGain, ctx.get(), {});
    TEST_CHECK(!gainRes.thrown);
    TEST_CHECK(ev::isObject(gainRes.value));
    ev::Persistent gain(gainRes.value);
    TEST_CHECK(ev::isObject(ev::getProperty(gain.get(), "gain")));
    TEST_CHECK(ev::isFunction(ev::getProperty(gain.get(), "connect")));
    TEST_CHECK(ev::isFunction(ev::getProperty(gain.get(), "disconnect")));
    Value gainValue = ev::getProperty(ev::getProperty(gain.get(), "gain"), "value");
    TEST_CHECK(ev::isNumber(gainValue));
    TEST_CHECK(ev::toDouble(gainValue) == 1.0);

    Value createOsc = ev::getProperty(ctx.get(), "createOscillator");
    ev::CallResult oscRes = ev::call(createOsc, ctx.get(), {});
    TEST_CHECK(!oscRes.thrown);
    TEST_CHECK(ev::isObject(oscRes.value));
    ev::Persistent osc(oscRes.value);
    TEST_CHECK(ev::isObject(ev::getProperty(osc.get(), "frequency")));
    TEST_CHECK(ev::isObject(ev::getProperty(osc.get(), "detune")));
    TEST_CHECK(ev::isString(ev::getProperty(osc.get(), "type")));
    TEST_CHECK(ev::isFunction(ev::getProperty(osc.get(), "start")));
    TEST_CHECK(ev::isFunction(ev::getProperty(osc.get(), "stop")));
    TEST_CHECK(ev::isFunction(ev::getProperty(osc.get(), "connect")));
    Value freqValue = ev::getProperty(ev::getProperty(osc.get(), "frequency"), "value");
    TEST_CHECK(ev::isNumber(freqValue));
    TEST_CHECK(ev::toDouble(freqValue) == 440.0);

    // connect chain: osc -> gain -> destination, each returning its argument.
    Value connectOsc = ev::getProperty(osc.get(), "connect");
    Value gainArg = gain.get();
    ev::CallResult c1 = ev::call(connectOsc, osc.get(), std::span<const Value>(&gainArg, 1));
    TEST_CHECK(!c1.thrown);
    TEST_CHECK(ev::isObject(c1.value));
    Value connectGain = ev::getProperty(gain.get(), "connect");
    Value destArg = ev::getProperty(ctx.get(), "destination");
    ev::CallResult c2 = ev::call(connectGain, gain.get(), std::span<const Value>(&destArg, 1));
    TEST_CHECK(!c2.thrown);

    // createBus() hands back a bus index.
    Value createBus = ev::getProperty(ctx.get(), "createBus");
    TEST_CHECK(ev::isFunction(createBus));
    ev::CallResult busRes = ev::call(createBus, ctx.get(), {});
    TEST_CHECK(!busRes.thrown);
    TEST_CHECK(ev::isNumber(busRes.value));
    TEST_CHECK(ev::toDouble(busRes.value) >= 0.0);

    // Bad arguments throw TypeError rather than silently succeeding.
    ev::CallResult badConnect = ev::call(connectGain, gain.get(), {});
    TEST_CHECK(badConnect.thrown);
    TEST_CHECK(errorName(badConnect.value) == "TypeError");

    Value bufCtor = ev::globalValue("AudioBuffer").value;
    ev::CallResult badBuf = ev::construct(bufCtor, {});
    TEST_CHECK(badBuf.thrown);
    TEST_CHECK(errorName(badBuf.value) == "TypeError");
}

static void test_graph_script() {
    std::cout << "[3/3] node graph via bronze eval..." << std::endl;

    const char* script = R"JS(
        (function() {
            const ctx = new AudioContext();
            if (!(ctx instanceof AudioContext)) throw new Error("ctx is not an AudioContext");
            if (typeof ctx.sampleRate !== "number" || !(ctx.sampleRate > 0)) throw new Error("sampleRate: " + ctx.sampleRate);
            if (typeof ctx.state !== "string") throw new Error("state: " + ctx.state);
            if (webkitAudioContext !== AudioContext) throw new Error("webkitAudioContext alias");

            const osc = ctx.createOscillator();
            const gain = ctx.createGain();
            const filter = ctx.createBiquadFilter();
            const analyser = ctx.createAnalyser();
            if (!(osc instanceof OscillatorNode)) throw new Error("createOscillator type");
            if (!(gain instanceof GainNode)) throw new Error("createGain type");
            if (!(osc instanceof AudioNode) || !(gain instanceof AudioNode)) throw new Error("AudioNode inheritance");
            if (!(gain.gain instanceof AudioParam)) throw new Error("gain.gain is not an AudioParam");
            if (osc.frequency.value !== 440) throw new Error("default frequency " + osc.frequency.value);
            if (typeof analyser.frequencyBinCount !== "number") throw new Error("frequencyBinCount");

            osc.type = "square";
            if (osc.type !== "square") throw new Error("type setter: " + osc.type);
            osc.frequency.value = 220;
            if (osc.frequency.value !== 220) throw new Error("frequency setter: " + osc.frequency.value);
            gain.gain.value = 0.5;
            if (gain.gain.value !== 0.5) throw new Error("gain setter: " + gain.gain.value);
            gain.gain.setValueAtTime(0.25, 0);
            gain.gain.linearRampToValueAtTime(0.75, 1);

            osc.connect(filter).connect(gain).connect(ctx.destination);
            gain.connect(analyser);
            gain.disconnect();

            const bus = ctx.createBus();
            if (typeof bus !== "number") throw new Error("createBus returned " + typeof bus);
            ctx.setBusGain(bus, 0.5);
            ctx.deleteBus(bus);

            const buf = new AudioBuffer({ length: 64, numberOfChannels: 2, sampleRate: 48000 });
            if (buf.length !== 64 || buf.numberOfChannels !== 2) throw new Error("AudioBuffer shape");
            const src = ctx.createBufferSource();
            src.buffer = buf;
            src.connect(gain);

            let threw = false;
            try { osc.connect(); } catch (e) { threw = e instanceof TypeError; }
            if (!threw) throw new Error("connect() with no destination should throw TypeError");
            threw = false;
            try { new AudioBuffer({ length: 0 }); } catch (e) { threw = e instanceof TypeError; }
            if (!threw) throw new Error("AudioBuffer with length 0 should throw TypeError");

            return "SUCCESS";
        })()
    )JS";

    ev::CallResult res = bronze::eval::evalScript(script);
    if (res.thrown) {
        std::cerr << "eval threw: " << ev::toUtf8(res.value) << std::endl;
        std::exit(1);
    }
    TEST_CHECK(ev::toUtf8(res.value) == "SUCCESS");
}

int main() {
    std::cout << "Running broaudio API test..." << std::endl;

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        test_mounts();
        test_context_direct();
        test_graph_script();
    }
    ev::destroyRealm(realm);

    std::cout << "All broaudio API tests passed!" << std::endl;
    return 0;
}
