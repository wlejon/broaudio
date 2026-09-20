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
            if (typeof ctx.isBusJitEnabled !== "function") throw new Error("missing isBusJitEnabled");
            if (typeof ctx.setBusJitEnabled !== "function") throw new Error("missing setBusJitEnabled");
            if (typeof ctx.isBusJitActive !== "function") throw new Error("missing isBusJitActive");
            if (!ctx.isBusJitEnabled(bus)) throw new Error("bus JIT should be enabled by default");
            ctx.setBusJitEnabled(bus, false);
            if (ctx.isBusJitEnabled(bus)) throw new Error("bus JIT should be disabled after setBusJitEnabled(false)");
            ctx.setBusJitEnabled(bus, true);
            if (!ctx.isBusJitEnabled(bus)) throw new Error("bus JIT should be enabled after setBusJitEnabled(true)");
            ctx.setBusGain(bus, 0.5);
            ctx.deleteBus(bus);

            const buf = new AudioBuffer({ length: 64, numberOfChannels: 2, sampleRate: 48000 });
            if (buf.length !== 64 || buf.numberOfChannels !== 2) throw new Error("AudioBuffer shape");
            const src = ctx.createBufferSource();
            src.buffer = buf;
            src.connect(gain);

            // Test 1: copyToChannel invalidates/updates cached TypedArray from getChannelData
            const testBuf = new AudioBuffer({ length: 16, numberOfChannels: 2, sampleRate: 44100 });
            const chData0 = testBuf.getChannelData(0);
            if (chData0[0] !== 0 || chData0[1] !== 0) throw new Error("buffer not zero-initialized");
            const newSamples = new Float32Array([0.125, 0.25, 0.5, 0.75]);
            testBuf.copyToChannel(newSamples, 0, 0);
            if (Math.abs(chData0[0] - 0.125) > 1e-5 || Math.abs(chData0[1] - 0.25) > 1e-5) {
                throw new Error("copyToChannel failed to synchronize cached channel data: " + chData0[0]);
            }
            const chDataAgain = testBuf.getChannelData(0);
            if (Math.abs(chDataAgain[2] - 0.5) > 1e-5 || Math.abs(chDataAgain[3] - 0.75) > 1e-5) {
                throw new Error("getChannelData after copyToChannel returned stale data");
            }

            // Test 2: AudioParam linear and exponential ramps and setTarget
            const testGain = ctx.createGain();
            testGain.gain.setValueAtTime(0.2, 0.0);
            testGain.gain.linearRampToValueAtTime(0.8, 2.0);

            if (Math.abs(testGain.gain.getValueAtTime(0.0) - 0.2) > 1e-4) throw new Error("linearRamp at t=0");
            if (Math.abs(testGain.gain.getValueAtTime(1.0) - 0.5) > 1e-4) throw new Error("linearRamp at t=1: " + testGain.gain.getValueAtTime(1.0));
            if (Math.abs(testGain.gain.getValueAtTime(2.0) - 0.8) > 1e-4) throw new Error("linearRamp at t=2");

            const expGain = ctx.createGain();
            expGain.gain.setValueAtTime(1.0, 0.0);
            expGain.gain.exponentialRampToValueAtTime(16.0, 4.0);
            if (Math.abs(expGain.gain.getValueAtTime(0.0) - 1.0) > 1e-4) throw new Error("exponentialRamp at t=0");
            if (Math.abs(expGain.gain.getValueAtTime(2.0) - 4.0) > 1e-4) throw new Error("exponentialRamp at t=2: " + expGain.gain.getValueAtTime(2.0));
            if (Math.abs(expGain.gain.getValueAtTime(4.0) - 16.0) > 1e-4) throw new Error("exponentialRamp at t=4");

            const targetGain = ctx.createGain();
            targetGain.gain.setValueAtTime(1.0, 0.0);
            targetGain.gain.setTargetAtTime(0.0, 0.0, 1.0);
            if (Math.abs(targetGain.gain.getValueAtTime(1.0) - 0.367879) > 1e-3) throw new Error("setTarget at t=1: " + targetGain.gain.getValueAtTime(1.0));

            // Test 3: AudioBufferSourceNode connected downstream settings (GainNode + PannerNode)
            const playBuf = new AudioBuffer({ length: 64, numberOfChannels: 1, sampleRate: 44100 });
            const playSrc = ctx.createBufferSource();
            playSrc.buffer = playBuf;
            const panner = ctx.createStereoPanner();
            panner.pan.value = 0.5;
            const routeGain = ctx.createGain();
            routeGain.gain.value = 0.4;
            playSrc.connect(panner).connect(routeGain).connect(ctx.destination);
            playSrc.start();

            // Test 4: DynamicsCompressorNode
            const comp = ctx.createDynamicsCompressor();
            if (!(comp instanceof DynamicsCompressorNode)) throw new Error("createDynamicsCompressor type");
            if (!(comp.threshold instanceof AudioParam) || !(comp.ratio instanceof AudioParam)) throw new Error("compressor params");
            if (typeof comp.reduction !== "number") throw new Error("compressor reduction not number");
            comp.threshold.value = -20;
            comp.ratio.value = 8;
            comp.attack.value = 0.001;
            comp.release.value = 0.05;

            // Connect buffer through compressor to verify graph wiring
            const compSrc = ctx.createBufferSource();
            const compBuf = new AudioBuffer({ length: 128, numberOfChannels: 1, sampleRate: 44100 });
            const cData = compBuf.getChannelData(0);
            for (let i = 0; i < 128; i++) cData[i] = 1.0;
            compSrc.buffer = compBuf;
            compSrc.connect(comp).connect(ctx.destination);
            compSrc.start();
            if (comp.reduction > 0) throw new Error("compressor reduction should be <= 0");

            // Test 5: StereoPannerNode prototype
            if (typeof Object.getOwnPropertyDescriptor(StereoPannerNode.prototype, "pan") === "undefined" &&
                typeof Object.getOwnPropertyDescriptor(Object.getPrototypeOf(panner), "pan") === "undefined") {
                throw new Error("StereoPannerNode prototype pan accessor missing");
            }
            if (typeof panner.pan.value !== "number") throw new Error("panner.pan.value missing");

            // Test 6: AudioContext lifecycle & state tracking
            if (ctx.state !== "running") throw new Error("AudioContext initial state should be running, got: " + ctx.state);
            ctx.suspend();
            if (ctx.state !== "suspended") throw new Error("AudioContext state after suspend should be suspended, got: " + ctx.state);
            ctx.resume();
            if (ctx.state !== "running") throw new Error("AudioContext state after resume should be running, got: " + ctx.state);
            const closePromise = ctx.close();
            if (ctx.state !== "closed") throw new Error("AudioContext state after close should be closed, got: " + ctx.state);
            if (!(closePromise instanceof Promise)) throw new Error("close() must return a Promise");

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
    broaudio::api::shutdownAudio();

    std::cout << "All broaudio API tests passed!" << std::endl;
    return 0;
}
