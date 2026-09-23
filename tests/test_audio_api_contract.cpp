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

// Argument validation and getter results the docs promise.
static void test_validation_and_defaults() {
    runScript("connect/start validation, buffer getters, buffer sample rates", withPrelude(R"JS(
        const src = ctx.createBufferSource();
        const conv = ctx.createConvolver();
        expect(src.buffer === null, "AudioBufferSourceNode.buffer starts null, got " + src.buffer);
        expect(conv.buffer === null, "ConvolverNode.buffer starts null, got " + conv.buffer);
        expectThrows(() => { src.buffer = {}; }, TypeError, "buffer = {} on a buffer source");
        expectThrows(() => { conv.buffer = new Float32Array(4); }, TypeError, "buffer = Float32Array on a convolver");
        const b = ctx.createBuffer(1, 64, sr);
        src.buffer = b;
        expect(src.buffer === b, "buffer reads back");
        src.buffer = null;
        expect(src.buffer === null, "buffer = null clears it");

        const osc = ctx.createOscillator();
        expectThrows(() => osc.connect({}), TypeError, "connect({})");
        expectThrows(() => osc.connect(undefined), TypeError, "connect(undefined)");
        expectThrows(() => osc.connect(42), TypeError, "connect(42)");
        const g = ctx.createGain();
        expect(osc.connect(g) === g, "connect returns its destination");
        osc.connect(g.gain);
        g.connect(ctx.destination);
        expectThrows(() => osc.start(-1), RangeError, "start(-1)");
        osc.start();                                   // the rejected start did not count
        expectThrows(() => osc.stop(-1), RangeError, "stop(-1)");
        ctx.renderBlock(256);
        osc.stop();
        ctx.renderBlock(256);

        expect(ctx.createBuffer(1, 10).sampleRate === sr, "createBuffer defaults to the context rate");
        expect(new AudioBuffer({ length: 10 }).sampleRate === sr, "new AudioBuffer defaults to the context rate");
        expect(ctx.createBuffer(1, 10, 22050).sampleRate === 22050, "an explicit rate is kept");

        // createClip(AudioBuffer) honours the buffer's rate.
        const half = ctx.createBuffer(1, 1000, sr / 2);
        const clip = ctx.createClip(half);
        const n = ctx.getClipSampleCount(clip);
        near(n, 2000, 4, "a half-rate buffer becomes a clip of twice the frames");
        ctx.deleteClip(clip);
        return "SUCCESS";
    )JS"));
}

// start() and stop() in the same tick: the stop is newer than the start, so
// the note ends (it used to be cleared by the start and ring on).
static void test_start_stop_same_tick() {
    runScript("OscillatorNode start + stop in one tick is silent", withPrelude(R"JS(
        const osc = ctx.createOscillator();
        osc.frequency.value = 440;
        osc.connect(ctx.destination);
        osc.start();
        osc.stop();
        ctx.renderBlock(4096);                     // the 40 ms release runs out
        const tail = peak(ctx.renderBlock(4096), 0, 4096);
        expect(tail < 1e-4, "start+stop in one tick left the voice sounding: " + tail);

        // A noteOff followed by a noteOn in one tick still retriggers.
        const v = ctx.createOscillator();
        v.connect(ctx.destination);
        v.start();
        ctx.renderBlock(1024);
        v.stop();
        const id = v.voiceId;
        ctx.startVoice(id);                        // restart after the release was issued
        ctx.renderBlock(4096);
        const held = peak(ctx.renderBlock(4096), 0, 4096);
        expect(held > 1e-2, "a start after a stop in one tick keeps the note: " + held);
        v.stop();
        ctx.renderBlock(4096);
        return "SUCCESS";
    )JS"));
}

// AudioBufferSourceNode.stop(when) is sample-accurate on the audio clock,
// and `ended` reaches onended plus addEventListener listeners.
static void test_buffer_source_stop_and_ended() {
    runScript("AudioBufferSourceNode stop(when), onended and addEventListener", withPrelude(R"JS(
        function toneBuffer(seconds) {
            const b = ctx.createBuffer(1, Math.round(seconds * sr), sr);
            const d = b.getChannelData(0);
            for (let i = 0; i < d.length; i++) d[i] = 0.5 * Math.sin(2 * Math.PI * 1000 * i / sr);
            return b;
        }
        // stop() before start() is Web Audio's InvalidStateError (a
        // DOMException where the realm has one, else an Error of that name),
        // checked before the argument.
        function expectInvalidState(fn, what) {
            let err = null;
            try { fn(); } catch (e) { err = e; }
            expect(err !== null, what + ": did not throw");
            expect(err.name === "InvalidStateError", what + ": threw " + err);
            if (typeof DOMException === "function") expect(err instanceof DOMException, what + ": not a DOMException");
        }
        expectInvalidState(() => ctx.createBufferSource().stop(), "unstarted buffer source stop()");
        expectInvalidState(() => ctx.createBufferSource().stop(-1), "unstarted buffer source stop(-1)");
        expectInvalidState(() => ctx.createOscillator().stop(), "unstarted oscillator stop()");
        const started = ctx.createBufferSource();
        started.start();                           // no buffer: started all the same
        expectThrows(() => started.stop(-1), RangeError, "stop(-1)");
        started.stop();

        // Scheduled stop 0.1 s in: sound up to that frame, silence after.
        const src = ctx.createBufferSource();
        src.buffer = toneBuffer(1);
        let ended = 0;
        src.onended = function (e) {
            ended++;
            expect(this === src && e.type === "ended" && e.target === src && e.currentTarget === src,
                   "ended event shape");
        };
        src.connect(ctx.destination);
        src.start();
        const stopFrame = Math.round(0.1 * sr);
        src.stop(ctx.currentTime + 0.1);
        const out = ctx.renderBlock(stopFrame + 2048);
        expect(peak(out, 0, stopFrame - 256) > 0.1, "plays until the stop time");
        expect(peak(out, stopFrame + 256, stopFrame + 2048) < 1e-4,
               "silent after the stop time: " + peak(out, stopFrame + 256, stopFrame + 2048));
        expect(ended === 1, "onended fired once the scheduled stop was reached: " + ended);
        ctx.renderBlock(1024);
        expect(ended === 1, "onended fires once");

        // A stop before the scheduled start: never sounds, still ends.
        const late = ctx.createBufferSource();
        late.buffer = toneBuffer(0.5);
        let lateEnded = 0;
        late.onended = () => { lateEnded++; };
        late.connect(ctx.destination);
        const t0 = ctx.currentTime;
        late.start(t0 + 0.05);
        late.stop(t0 + 0.02);
        const quiet = ctx.renderBlock(Math.round(0.1 * sr));
        expect(peak(quiet, 0, quiet.length) < 1e-4, "a stop before the start never sounds");
        expect(lateEnded === 1, "and still fires onended: " + lateEnded);

        // addEventListener: deduplicated, `once`, removable; after onended.
        const ev = ctx.createBufferSource();
        ev.buffer = toneBuffer(0.02);
        const calls = [];
        const f = function (e) { calls.push("f:" + e.type + ":" + (this === ev)); };
        const gone = () => calls.push("gone");
        ev.onended = () => calls.push("on");
        ev.addEventListener("ended", f);
        ev.addEventListener("ended", f);           // same pair: ignored
        ev.addEventListener("ended", () => calls.push("once"), { once: true });
        ev.addEventListener("ended", gone);
        ev.removeEventListener("ended", gone);
        ev.start();
        ctx.renderBlock(4096);
        expect(calls.join() === "on,f:ended:true,once", "dispatch: " + calls.join());
        return "SUCCESS";
    )JS"));
}

// A buffer source's start() binds Delay / DynamicsCompressor params to the
// master bus, as an oscillator's does: later sets reach the bus.
static void test_buffer_source_binds_master_effects() {
    runScript("AudioBufferSourceNode start binds delay and compressor params live", withPrelude(R"JS(
        const b = ctx.createBuffer(1, sr, sr);
        const src = ctx.createBufferSource();
        src.buffer = b;
        const delay = ctx.createDelay(2);
        const comp = ctx.createDynamicsCompressor();
        delay.delayTime.value = 0.2;
        src.connect(delay).connect(comp).connect(ctx.destination);
        const before = ctx.getBusDelayTime(0);
        src.start();
        delay.delayTime.value = 0.4;
        near(ctx.getBusDelayTime(0) / before, 2, 0.01, "delayTime is live on the master bus");
        comp.ratio.value = 4;
        near(ctx.getBusCompressorRatio(0), 4, 1e-4, "compressor ratio is live on the master bus");
        src.stop();
        delay.disconnect();
        comp.disconnect();
        ctx.renderBlock(1024);
        return "SUCCESS";
    )JS"));
}

// A handle some other library made: its payload is not broaudio's, but it
// leads with `tag`, so a binding that trusts the payload's leading bytes (what
// every xxxOf helper used to do after ev::handleData) takes it for one of
// its own. The rest of the payload is zero.
struct ForgedPayload {
    uint32_t tag;
    unsigned char rest[252];
};

static void installForger() {
    ev::Persistent fn(ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        auto* p = new ForgedPayload{};
        p->tag = a.empty() ? 0u : static_cast<uint32_t>(ev::toDouble(a[0]));
        return ev::makeHandle(p, [](void* d) { delete static_cast<ForgedPayload*>(d); });
    }, 1, "forgeHandle"));
    ev::registerGlobal("forgeHandle", fn.get());
    ev::GlobalValue g = ev::globalValue("globalThis");
    TEST_CHECK(g.found);
    ev::Persistent global(g.value);
    ev::setProperty(global.get(), "forgeHandle", fn.get());
}

// Every receiver and handle argument is checked against the class that made
// it (host_class.cpp, brands): a handle of another class, or another
// library's handle that happens to lead with a broaudio tag, is refused.
static void test_wrong_receiver() {
    runScript("methods refuse handles of another class or library", withPrelude(R"JS(
        const TAGS = { ACTX: 0x41435458, ANOD: 0x414E4F44, APAR: 0x41504152,
                       ABUF: 0x41425546, PWAV: 0x50574156, VALC: 0x56414C43,
                       MODM: 0x4D4F444D, MIDI: 0x4D494449, SEQU: 0x53455155,
                       MSTR: 0x4D535452 };
        const forged = {};
        for (const k in TAGS) forged[k] = forgeHandle(TAGS[k]);

        // A forged context is not a context: `state` answers the unbranded
        // default rather than reading a std::string out of foreign memory.
        const stateGet = Object.getOwnPropertyDescriptor(AudioContext.prototype, "state").get;
        expect(stateGet.call(forged.ACTX) === "running", "forged context state");
        ctx.suspend.call(forged.ACTX);
        expect(ctx.state === "running", "suspend on a forged context leaves the real one alone");

        const buf = ctx.createBuffer(1, 128, sr);
        const gain = ctx.createGain();
        const osc = ctx.createOscillator();
        const wave = ctx.createPeriodicWave(new Float32Array([0, 1]), new Float32Array([0, 0]));

        // Arguments: a PeriodicWave slot takes only a PeriodicWave.
        expectThrows(() => osc.setPeriodicWave(forged.PWAV), TypeError, "forged PeriodicWave");
        expectThrows(() => osc.setPeriodicWave(buf), TypeError, "AudioBuffer as PeriodicWave");
        osc.setPeriodicWave(wave);
        // connect() takes a node or a param, not a lookalike.
        expectThrows(() => gain.connect(forged.ANOD), TypeError, "forged node");
        expectThrows(() => gain.connect(forged.APAR), TypeError, "forged param");
        expectThrows(() => gain.connect(buf), TypeError, "AudioBuffer as a node");

        // Receivers: an AudioBuffer method on a node or a forged buffer.
        expect(AudioBuffer.prototype.getChannelData.call(gain, 0) === undefined, "buffer method on a node");
        expect(AudioBuffer.prototype.getChannelData.call(forged.ABUF, 0) === undefined, "buffer method on a forged buffer");

        // Every method and getter of every class, on every wrong receiver,
        // with no arguments: nothing may crash or read a foreign payload.
        // Names that reach files, devices or dialogs are left out.
        const skip = /^(constructor|save|export|open|close|decode|load|createClipFromFile|createStreamFromFile|createMediaStreamSource|createMidiInput|startRecording)/;
        const classes = [AudioContext, AudioNode, AudioParam, AudioBuffer, GainNode,
                         OscillatorNode, AudioBufferSourceNode, BiquadFilterNode,
                         AnalyserNode, PannerNode, StereoPannerNode, DelayNode,
                         DynamicsCompressorNode, WaveShaperNode, ConvolverNode,
                         ChannelSplitterNode, ChannelMergerNode, PeriodicWave];
        const receivers = [...Object.values(forged), buf, wave, gain.gain, {}, 1, undefined];
        let calls = 0;
        for (const C of classes) {
            const proto = C.prototype;
            for (const name of Object.getOwnPropertyNames(proto)) {
                if (skip.test(name)) continue;
                const d = Object.getOwnPropertyDescriptor(proto, name);
                const fn = typeof d.value === "function" ? d.value : d.get;
                if (typeof fn !== "function") continue;
                for (const r of receivers) {
                    if (r === buf && C === AudioBuffer) continue;
                    if (r === wave && C === PeriodicWave) continue;
                    if (r === gain.gain && C === AudioParam) continue;
                    try { fn.call(r); } catch (e) {}
                    calls++;
                }
            }
        }
        expect(calls > 500, "swept the prototypes (" + calls + " calls)");
        ctx.renderBlock(1024);
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
        test_validation_and_defaults();
        test_start_stop_same_tick();
        test_buffer_source_stop_and_ended();
        test_buffer_source_binds_master_effects();
        installForger();
        test_wrong_receiver();
        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio API contract tests passed!" << std::endl;
    return 0;
}
