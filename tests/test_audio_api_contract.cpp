// The binding against its documented contract (bro's docs/audio-api.js and
// audio-engine-api.js): units that cross from Web Audio into the engine, what
// a node does to the sound just by existing, and argument validation.
// Registered plainly and under BRONZE_GC_STRESS=1 + BRONZE_GC_POISON=1
// (tests/CMakeLists.txt).
//
// Runs on its own headless broaudio::Engine; ctx.renderBlock() drives it.

#include "api.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "broaudio/engine.h"

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

static void runScript(const char* label, const std::string& script) {
    std::cout << "  " << label << "..." << std::endl;
    ev::CallResult res = bronze::eval::evalScript(script);
    if (res.thrown) {
        std::cerr << label << " threw: " << ev::toUtf8(res.value) << std::endl;
        std::exit(1);
    }
    if (ev::toUtf8(res.value) != "SUCCESS") {
        std::cerr << label << " returned: " << ev::toUtf8(res.value) << std::endl;
        std::exit(1);
    }
}

static const char* kPrelude = R"JS(
    const ctx = new AudioContext();
    const sr = ctx.sampleRate;
    function near(a, b, eps, what) {
        if (!(Math.abs(a - b) <= eps)) throw new Error(what + ": " + a + " != " + b);
    }
    function expect(cond, what) { if (!cond) throw new Error(what); }
    function expectThrows(fn, ctor, what) {
        let err = null;
        try { fn(); } catch (e) { err = e; }
        if (!err) throw new Error(what + ": did not throw");
        if (!(err instanceof ctor)) throw new Error(what + ": threw " + err + ", not " + ctor.name);
    }
    function peak(out, from, to) {
        let p = 0;
        for (let i = from; i < to; i++) p = Math.max(p, Math.abs(out[i]));
        return p;
    }
)JS";

static std::string withPrelude(const char* body) {
    return std::string("(function() {") + kPrelude + body + "\n})()";
}

// DynamicsCompressorNode.threshold is dB; the bus compressor takes a linear
// level. The node's -24 dB default used to reach the bus as -24, which the
// bus clamps to 0 -- a compressor that squashes everything.
static void test_compressor_units() {
    runScript("DynamicsCompressorNode params reach the master compressor in its units", withPrelude(R"JS(
        const comp = ctx.createDynamicsCompressor();
        near(comp.threshold.value, -24, 1e-6, "threshold default (dB)");
        near(comp.threshold.minValue, -100, 1e-6, "threshold min");
        near(comp.threshold.maxValue, 0, 1e-6, "threshold max");
        near(comp.knee.value, 30, 1e-6, "knee default");
        near(comp.knee.maxValue, 40, 1e-6, "knee max");
        near(comp.ratio.value, 12, 1e-6, "ratio default");
        near(comp.ratio.minValue, 1, 1e-6, "ratio min");
        near(comp.ratio.maxValue, 20, 1e-6, "ratio max");
        near(comp.attack.value, 0.003, 1e-6, "attack default (s)");
        near(comp.release.value, 0.25, 1e-6, "release default (s)");

        comp.threshold.value = -12;
        comp.connect(ctx.destination);
        expect(ctx.getBusCompressorEnabled(0), "connect enables the master compressor");
        near(ctx.getBusCompressorThreshold(0), Math.pow(10, -12 / 20), 1e-4, "-12 dB as a linear level");
        near(ctx.getBusCompressorRatio(0), 12, 1e-4, "ratio");
        near(ctx.getBusCompressorAttack(0), 3, 1e-3, "attack in ms");
        near(ctx.getBusCompressorRelease(0), 250, 1e-3, "release in ms");

        // A started path binds the params to the master bus: later sets and
        // automation reach it, in the bus's units.
        const osc = ctx.createOscillator();
        osc.connect(comp);
        osc.start();
        near(ctx.getBusCompressorThreshold(0), Math.pow(10, -12 / 20), 1e-4, "start() keeps the threshold linear");
        comp.threshold.value = -40;
        near(ctx.getBusCompressorThreshold(0), Math.pow(10, -40 / 20), 1e-5, "live threshold set");
        comp.threshold.setValueAtTime(-6, 0);
        near(ctx.getBusCompressorThreshold(0), Math.pow(10, -6 / 20), 1e-4, "threshold automation");
        comp.release.value = 0.5;
        near(ctx.getBusCompressorRelease(0), 500, 1e-3, "live release in ms");
        ctx.renderBlock(1024);
        osc.stop();
        comp.disconnect();
        expect(!ctx.getBusCompressorEnabled(0), "disconnect disables it");
        ctx.renderBlock(1024);
        return "SUCCESS";
    )JS"));
}

// A BiquadFilterNode owns a master filter slot, but creating one must not
// change the sound: the slot runs only while the node is connected.
static void test_biquad_off_until_connected() {
    runScript("BiquadFilterNode filters only while connected", withPrelude(R"JS(
        function enabledSlots() {
            let n = 0;
            for (let s = 0; s < 4; s++) if (ctx.getBusFilterEnabled(0, s)) n++;
            return n;
        }
        function level() {
            ctx.renderBlock(4096);
            return peak(ctx.renderBlock(4096), 0, 4096);
        }
        const osc = ctx.createOscillator();
        osc.frequency.value = 5000;
        osc.connect(ctx.destination);
        osc.start();
        const open = level();
        expect(open > 1e-3, "5 kHz tone audible: " + open);

        const before = enabledSlots();
        const f = ctx.createBiquadFilter();       // lowpass 350 Hz
        expect(enabledSlots() === before, "creating the node leaves its slot off");
        near(level() / open, 1, 0.1, "creating a lowpass leaves a 5 kHz tone alone");

        f.connect(ctx.destination);
        expect(enabledSlots() === before + 1, "connect() switches the slot on");
        const cut = level();
        expect(cut < open * 0.25, "connected, the lowpass cuts 5 kHz: " + cut + " vs " + open);

        f.disconnect();
        expect(enabledSlots() === before, "disconnect() switches it off again");
        near(level() / open, 1, 0.1, "disconnected, the tone is back");
        osc.stop();
        ctx.renderBlock(4096);
        return "SUCCESS";
    )JS"));
}

// A Float32Array parameter given another typed array, or one whose buffer was
// detached, is a TypeError: the bytes are never reinterpreted as floats.
// Parameters documented to also take a plain array of numbers still do.
static void test_typed_array_args() {
    runScript("Float32Array parameters reject other element types and detached buffers", withPrelude(R"JS(
        function detached() {
            const f = new Float32Array(8);
            f.buffer.transfer();
            return f;
        }
        const wrong = () => new Int16Array(8);
        function rejects(label, call) {
            expectThrows(() => call(wrong()), TypeError, label + " with an Int16Array");
            expectThrows(() => call(detached()), TypeError, label + " with a detached Float32Array");
        }

        // Output arrays, written in place.
        const an = ctx.createAnalyser();
        rejects("getFloatTimeDomainData", (x) => an.getFloatTimeDomainData(x));
        rejects("getFloatFrequencyData", (x) => an.getFloatFrequencyData(x));
        expectThrows(() => an.getFloatTimeDomainData([0, 0]), TypeError, "getFloatTimeDomainData with a plain array");
        expectThrows(() => an.getByteTimeDomainData(new Float32Array(8)), TypeError,
                     "getByteTimeDomainData with a Float32Array");
        expectThrows(() => an.getByteFrequencyData(new Int8Array(8)), TypeError,
                     "getByteFrequencyData with an Int8Array");
        an.getFloatTimeDomainData(new Float32Array(an.fftSize));
        an.getByteFrequencyData(new Uint8Array(an.frequencyBinCount));

        const bq = ctx.createBiquadFilter();
        const hz = new Float32Array([100, 1000]);
        rejects("getFrequencyResponse mag", (x) => bq.getFrequencyResponse(hz, x, new Float32Array(2)));
        rejects("getFrequencyResponse phase", (x) => bq.getFrequencyResponse(hz, new Float32Array(2), x));
        rejects("getFrequencyResponse frequencyHz", (x) => bq.getFrequencyResponse(x, new Float32Array(2), new Float32Array(2)));
        const mag = new Float32Array(2), phase = new Float32Array(2);
        bq.getFrequencyResponse([100, 1000], mag, phase);
        expect(mag[0] > 0.5 && mag[1] < mag[0], "plain frequencyHz accepted: " + mag[0] + ", " + mag[1]);

        const buf = ctx.createBuffer(1, 16, sr);
        rejects("copyFromChannel", (x) => buf.copyFromChannel(x, 0));
        rejects("copyToChannel", (x) => buf.copyToChannel(x, 0));
        expectThrows(() => buf.copyToChannel([1, 2], 0), TypeError, "copyToChannel with a plain array");

        // Inputs that also take a plain array of numbers.
        rejects("createPeriodicWave real", (x) => ctx.createPeriodicWave(x, new Float32Array(8)));
        rejects("createPeriodicWave imag", (x) => ctx.createPeriodicWave(new Float32Array(8), x));
        ctx.createPeriodicWave([0, 1], [0, 0]);
        ctx.createPeriodicWave(new Float32Array([0, 1, 0.5]), new Float32Array([0, 0]));
        rejects("new PeriodicWave(ctx, {real})", (x) => new PeriodicWave(ctx, { real: x, imag: [0, 0] }));
        rejects("new PeriodicWave(real, imag)", (x) => new PeriodicWave(x, [0, 0]));
        const pw = new PeriodicWave(ctx, { real: [0, 1], imag: [0, 0], disableNormalization: true });
        const osc = ctx.createOscillator();
        osc.setPeriodicWave(pw);
        osc.setPeriodicWave(new PeriodicWave(ctx, { imag: [0, 1] }));
        osc.setPeriodicWave(new PeriodicWave([0, 1], [0, 0]));

        const ws = ctx.createWaveShaper();
        rejects("WaveShaper curve", (x) => { ws.curve = x; });
        ws.curve = [-1, 0, 1];
        ws.curve = null;

        const g = ctx.createGain();
        rejects("setValueCurveAtTime", (x) => g.gain.setValueCurveAtTime(x, ctx.currentTime, 0.1));
        g.gain.setValueCurveAtTime([0, 1], ctx.currentTime, 0.1);

        // Engine-level inputs.
        rejects("createClip", (x) => ctx.createClip(x, 1));
        expectThrows(() => ctx.createClip([0, 1], 1), TypeError, "createClip with a plain array");
        rejects("createWavetableFromWaveform", (x) => ctx.createWavetableFromWaveform(x));
        const stream = ctx.createStream(1, 1024);
        rejects("pushStreamSamples", (x) => ctx.pushStreamSamples(stream, x));
        ctx.closeStream(stream);
        rejects("processEffectsOffline", (x) => ctx.processEffectsOffline(0, x));
        rejects("saveWav", (x) => ctx.saveWav("never-written.wav", x, 1, sr));

        const t0 = ctx.currentTime;
        rejects("renderBlock out", (x) => ctx.renderBlock(128, x));
        expect(ctx.currentTime === t0, "a rejected renderBlock does not advance the clock");
        const out = new Float32Array(128);
        expect(ctx.renderBlock(128, out) === out, "renderBlock fills a Float32Array out in place");
        expect(ctx.renderBlock(128, null) instanceof Float32Array, "null out returns a fresh array");

        const midi = ctx.createMidiInput();
        const bytes = new Uint8Array([0x90, 60, 100]);
        bytes.buffer.transfer();
        expectThrows(() => midi.injectMessage(bytes), TypeError, "injectMessage with a detached Uint8Array");
        return "SUCCESS";
    )JS"));
}

int main() {
    std::cout << "Running broaudio API contract tests..." << std::endl;
    const char* stress = std::getenv("BRONZE_GC_STRESS");
    std::cout << "  (BRONZE_GC_STRESS=" << (stress ? stress : "unset") << ")" << std::endl;

    broaudio::Engine engine;
    TEST_CHECK(engine.initHeadless());
    broaudio::api::setAudioEngine(&engine);

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        test_compressor_units();
        test_biquad_off_until_connected();
        test_typed_array_args();
        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio API contract tests passed!" << std::endl;
    return 0;
}
