// broaudio_api under bronze's moving collector: every binding path that
// holds a Value across an allocating embed call (embed.h's GC contract), plus
// the functional fixes that came with that audit. Registered twice in
// tests/CMakeLists.txt, once plainly and once under BRONZE_GC_STRESS=1 +
// BRONZE_GC_POISON=1 (collect on every allocation, poison from-space), where
// a stale Value turns into a crash or a wrong answer instead of passing by
// luck.
//
// Runs on its own headless broaudio::Engine so renderBlock() and the mic
// injection seam drive the pipeline deterministically.

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
    function near(a, b, eps, what) {
        if (!(Math.abs(a - b) <= eps)) throw new Error(what + ": " + a + " != " + b);
    }
    function expect(cond, what) { if (!cond) throw new Error(what); }
    // Churn the heap from inside a callback the binding runs mid-call.
    function churn() { const junk = []; for (let i = 0; i < 64; i++) junk.push({ i: i, s: "x" + i }); return junk.length; }
    // A mono 16-bit PCM WAV of `frames` samples of a 441 Hz sine.
    function wavBytes(frames, rate) {
        const bytes = new Uint8Array(44 + frames * 2);
        const dv = new DataView(bytes.buffer);
        const str = (off, s) => { for (let i = 0; i < s.length; i++) bytes[off + i] = s.charCodeAt(i); };
        str(0, "RIFF"); dv.setUint32(4, 36 + frames * 2, true); str(8, "WAVE");
        str(12, "fmt "); dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 1, true);
        dv.setUint32(24, rate, true); dv.setUint32(28, rate * 2, true); dv.setUint16(32, 2, true); dv.setUint16(34, 16, true);
        str(36, "data"); dv.setUint32(40, frames * 2, true);
        for (let i = 0; i < frames; i++) dv.setInt16(44 + i * 2, Math.round(16000 * Math.sin(2 * Math.PI * 441 * i / rate)), true);
        return bytes;
    }
)JS";

static std::string withPrelude(const char* body) {
    return std::string("(function() {") + kPrelude + body + "\n})()";
}

static void test_buffers() {
    runScript("AudioBuffer channel data", withPrelude(R"JS(
        const opts = { get length() { churn(); return 32; }, get numberOfChannels() { churn(); return 2; },
                       get sampleRate() { churn(); return ctx.sampleRate; } };
        const buf = new AudioBuffer(opts);
        expect(buf.length === 32 && buf.numberOfChannels === 2, "AudioBuffer options through getters");

        const ch0 = buf.getChannelData(0);
        expect(ch0 instanceof Float32Array && ch0.length === 32, "getChannelData shape");
        expect(buf.getChannelData(0) === ch0, "getChannelData returns the cached view");
        // The cache is the binding's, not a property a script can see or break.
        expect(Object.keys(buf).length === 0 && buf._ch0 === undefined, "no script-visible channel cache");
        buf._ch0 = new Float32Array(32);
        expect(buf.getChannelData(0) === ch0, "a script property does not replace the cached view");

        // Writes into the view are what copyFromChannel reads back.
        ch0[3] = 0.5;
        const out = new Float32Array(8);
        buf.copyFromChannel(out, 0, 0);
        near(out[3], 0.5, 1e-6, "copyFromChannel sees getChannelData writes");

        // copyToChannel refreshes the cached view in place.
        buf.copyToChannel(new Float32Array([0.25, -0.25]), 0, 10);
        near(ch0[10], 0.25, 1e-6, "copyToChannel -> cached view [10]");
        near(ch0[11], -0.25, 1e-6, "copyToChannel -> cached view [11]");
        buf.copyToChannel(new Float32Array([1, 2]), 0, -1);  // negative offset: ignored
        near(ch0[0], 0, 1e-6, "negative startInChannel is ignored");

        // A two-channel AudioBuffer clip reads both cached channels.
        buf.getChannelData(1)[0] = 0.75;
        const clip = ctx.createClip(buf);
        expect(ctx.getClipChannels(clip) === 2, "createClip(AudioBuffer) channels: " + ctx.getClipChannels(clip));
        expect(ctx.getClipSampleCount(clip) === 32, "createClip(AudioBuffer) frames: " + ctx.getClipSampleCount(clip));
        ctx.deleteClip(clip);

        const src = ctx.createBufferSource();
        src.buffer = buf;
        expect(src.buffer === buf, "AudioBufferSourceNode.buffer round-trip");
        const conv = ctx.createConvolver();
        conv.buffer = buf;
        expect(conv.buffer === buf, "ConvolverNode.buffer round-trip");
        return "SUCCESS";
    )JS"));
}

static void test_decode() {
    runScript("decodeAudioData callbacks and promise fields", withPrelude(R"JS(
        const bytes = wavBytes(441, 44100);
        let got = null, err = null;
        const p = ctx.decodeAudioData(bytes.buffer, (b) => { churn(); got = b; }, (e) => { err = e; });
        expect(err === null, "valid WAV reported an error: " + err);
        expect(got instanceof AudioBuffer, "success callback gets an AudioBuffer");
        expect(got.numberOfChannels === 1, "decoded channels " + got.numberOfChannels);
        expect(p instanceof Promise, "decodeAudioData returns a Promise");
        expect(p.samples instanceof Float32Array && p.samples.length === p.numFrames, "promise samples field");
        expect(got.samples === p.samples, "the AudioBuffer and the promise share one samples array");
        expect(p.channels === 1 && p.sampleRate === ctx.sampleRate, "promise channels/sampleRate");
        const peak = p.samples.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
        expect(peak > 0.3 && peak < 0.6, "decoded peak " + peak);

        let bad = null;
        const p2 = ctx.decodeAudioData(new Uint8Array([1, 2, 3, 4]).buffer, () => { throw new Error("success on garbage"); },
                                       (e) => { churn(); bad = e; });
        expect(bad instanceof Error, "error callback gets an Error");
        expect(bad.name === "EncodingError", "error name " + bad.name);
        expect(p2 instanceof Promise, "failed decode still returns the Promise");
        p2.catch(() => {});
        expect(ctx.decodeAudioData(new Uint8Array([1, 2, 3]).buffer) === null, "no callbacks + garbage -> null");
        return "SUCCESS";
    )JS"));
}

static void test_nodes() {
    runScript("node params", withPrelude(R"JS(
        const g = ctx.createGain();
        expect(g.gain instanceof AudioParam && g.gain.value === 1, "GainNode.gain");

        const comp = ctx.createDynamicsCompressor();
        const want = { threshold: -24, knee: 30, ratio: 12, attack: 0.003, release: 0.25 };
        for (const k in want) {
            expect(comp[k] instanceof AudioParam, "compressor." + k + " is an AudioParam");
            near(comp[k].value, want[k], 1e-6, "compressor." + k);
        }

        const sp = ctx.createStereoPanner();
        expect(sp.pan instanceof AudioParam && sp.pan.value === 0, "StereoPannerNode.pan");

        const pn = ctx.createPanner();
        for (const k of ["positionX", "positionY", "positionZ", "orientationX", "orientationY", "orientationZ"]) {
            expect(pn[k] instanceof AudioParam, "panner." + k);
        }
        pn.setPosition(1, 2, 3);
        expect(pn.positionX.value === 1 && pn.positionY.value === 2 && pn.positionZ.value === 3, "setPosition -> position params");
        pn.setOrientation(0, 0, -1);
        expect(pn.orientationX.value === 0 && pn.orientationZ.value === -1, "setOrientation -> orientation params");

        // setValueCurveAtTime with a plain array returns the param itself.
        const p = ctx.createGain().gain;
        expect(p.setValueCurveAtTime([0, 1, 0.5], 0, 1) === p, "setValueCurveAtTime chains");
        near(p.getValueAtTime(0.5), 1, 1e-4, "curve midpoint");

        const osc = ctx.createOscillator();
        osc.connect(g).connect(pn).connect(ctx.destination);
        pn.positionX.value = 5;
        osc.start();
        osc.stop();
        return "SUCCESS";
    )JS"));

    runScript("PeriodicWave and getFrequencyResponse", withPrelude(R"JS(
        const opts = { get disableNormalization() { churn(); return true; } };
        const w1 = ctx.createPeriodicWave(new Float32Array([0, 1, 0.5]), new Float32Array([0, 0, 0]), opts);
        expect(w1 instanceof PeriodicWave, "createPeriodicWave");
        const w2 = new PeriodicWave(new Float32Array([0, 1, 0.5, 0.25]), new Float32Array([0, 0]), opts);
        expect(w2 instanceof PeriodicWave, "PeriodicWave with unequal halves");
        const osc = ctx.createOscillator();
        osc.setPeriodicWave(w2);
        expect(osc.type === "custom", "setPeriodicWave -> custom");

        const f = ctx.createBiquadFilter();  // lowpass 350 Hz
        const freqs = [50, 350, 5000];
        const mag = new Float32Array(3), phase = new Float32Array(3);
        f.getFrequencyResponse(freqs, mag, phase);
        expect(mag[0] > 0.9 && mag[2] < 0.1, "lowpass response " + mag[0] + " / " + mag[2]);
        const at350 = mag[1];
        f.detune.value = 1200;  // one octave up: 700 Hz
        f.getFrequencyResponse(new Float32Array(freqs), mag, phase);
        expect(mag[1] > at350 + 0.1, "detune raises the cutoff: " + mag[1] + " vs " + at350);
        return "SUCCESS";
    )JS"));
}

static void test_sequencer() {
    runScript("voice setup through noteOn and Sequence.update", withPrelude(R"JS(
        const va = ctx.createVoiceAllocator(8);
        const seen = [];
        va.setVoiceSetup((voice, note, vel) => { churn(); seen.push(note); });
        va.noteOn(64, 1);
        va.noteOn(65, 1);
        expect(seen.join(",") === "64,65", "noteOn voice setup: " + seen.join(","));
        va.allNotesOff();

        const seq = ctx.createSequence(va);
        seq.setBPM(120);
        seq.addNote(0, 60, 1, 0.25);
        const t0 = ctx.currentTime;
        seq.play(t0);
        seq.update(t0 + 0.01);
        expect(seen.indexOf(60) >= 0, "Sequence.update ran the allocator's voice setup: " + seen.join(","));
        seq.stop();
        return "SUCCESS";
    )JS"));

    runScript("automation lanes keep their callbacks across removal", withPrelude(R"JS(
        const seq = ctx.createSequence(ctx.createVoiceAllocator(4));
        const calls = [];
        const a = seq.addAutomationLane((v) => { churn(); calls.push("a"); });
        const b = seq.addAutomationLane((v) => { churn(); calls.push("b:" + v); });
        expect(a === 0 && b === 1, "lane indices " + a + "," + b);
        seq.addAutomationPoint(0, 0, 0.1);
        seq.addAutomationPoint(1, 0, 0.5);
        seq.removeAutomationLane(0);
        expect(seq.automationLaneCount === 1, "lane count after removal");
        const t0 = ctx.currentTime;
        seq.play(t0);
        seq.update(t0 + 0.01);
        expect(calls.length === 1 && calls[0] === "b:0.5", "after removing lane 0 the old lane 1 calls b: " + calls.join(","));
        return "SUCCESS";
    )JS"));
}

// A full collection with nothing of the script's on the stack: drain the
// microtask checkpoint first so WeakRef targets kept alive for the current
// job are released, then collect and run deferred finalizers.
static void collectNow() {
    ev::drainMicrotasks();
    ev::collectGarbage();
    ev::drainFinalizers();
    ev::collectGarbage();
}

static void test_param_ownership() {
    // A node reads its params' state directly (start(), getFrequencyResponse,
    // the live-param refresh). The JS AudioParam objects are ordinary
    // properties a script can overwrite; once one is unreachable and
    // collected, the node must still own the state it reads.
    runScript("params outlive their JS objects: setup", withPrelude(R"JS(
        const g = ctx.createGain();
        g.gain.value = 0.25;
        const f = ctx.createBiquadFilter();
        f.frequency.value = 500;
        f.detune.value = 1200;
        const src = ctx.createBufferSource();
        src.playbackRate.value = 2;
        const buf = ctx.createBuffer(1, 256, ctx.sampleRate);
        buf.getChannelData(0).fill(0.5);
        src.buffer = buf;
        const conv = ctx.createConvolver();
        conv.buffer = ctx.createBuffer(1, 32, ctx.sampleRate);
        g.gain = null;
        f.frequency = null;
        f.detune = null;
        src.playbackRate = null;
        globalThis.__owned = { g, f, src, conv };
        return "SUCCESS";
    )JS"));
    collectNow();
    runScript("params outlive their JS objects: use", withPrelude(R"JS(
        // Reuse the freed memory with other values: a dangling read would
        // now see 7 instead of the node's own 0.25 / 500 Hz.
        const junk = [];
        for (let i = 0; i < 2000; i++) { const p = ctx.createGain().gain; p.value = 7; junk.push(p); }
        const { g, f, src } = globalThis.__owned;

        const osc = ctx.createOscillator();
        osc.connect(g).connect(ctx.destination);
        osc.start();
        const voice = osc.voiceId;
        expect(voice >= 0, "oscillator voice");
        const out = ctx.renderBlock(4096);
        let peak = 0;
        for (let i = 2048; i < out.length; i++) peak = Math.max(peak, Math.abs(out[i]));
        osc.stop();
        ctx.renderBlock(4096);

        const ref = ctx.createOscillator();
        const g2 = ctx.createGain();
        g2.gain.value = 0.25;
        ref.connect(g2).connect(ctx.destination);
        ref.start();
        const out2 = ctx.renderBlock(4096);
        let refPeak = 0;
        for (let i = 2048; i < out2.length; i++) refPeak = Math.max(refPeak, Math.abs(out2[i]));
        ref.stop();
        ctx.renderBlock(4096);
        near(peak, refPeak, refPeak * 0.05 + 1e-4, "orphaned gain param still 0.25");

        // 500 Hz detuned an octave is a 1 kHz lowpass: |H| = Q = 1 at the
        // cutoff, about (1k/5k)^2 two octaves above it.
        const mag = new Float32Array(2), phase = new Float32Array(2);
        f.getFrequencyResponse(new Float32Array([1000, 5000]), mag, phase);
        expect(mag[0] > 0.9 && mag[0] < 1.1 && mag[1] > 0.02 && mag[1] < 0.1,
               "orphaned filter params: " + mag[0] + " / " + mag[1]);

        src.connect(ctx.destination);
        src.start();
        return "SUCCESS";
    )JS"));
}

static void test_cycles_collect() {
    // Edges the binding keeps for a node -- connect() targets, automation
    // lane callbacks -- must be edges the collector can see. Held as host
    // roots, a cycle through them (a feedback loop, a callback closing over
    // its own sequence) would keep the whole graph alive forever.
    runScript("reference cycles through nodes: build", withPrelude(R"JS(
        const a = ctx.createGain(), d = ctx.createDelay(), f = ctx.createBiquadFilter();
        a.connect(d); d.connect(f); f.connect(a);          // feedback loop
        a.connect(ctx.destination);
        globalThis.__loop = new WeakRef(a);

        const seq = ctx.createSequence(ctx.createVoiceAllocator(2));
        seq.addAutomationLane((v) => { seq.lastValue = v; });  // closes over seq
        globalThis.__seq = new WeakRef(seq);

        // A connected pair that stays reachable must keep its edge.
        const keepA = ctx.createGain(), keepB = ctx.createGain();
        keepA.connect(keepB);
        globalThis.__kept = keepA;
        globalThis.__keptTarget = new WeakRef(keepB);
        return "SUCCESS";
    )JS"));
    collectNow();
    runScript("reference cycles through nodes: collected", withPrelude(R"JS(
        expect(globalThis.__loop.deref() === undefined, "feedback loop of nodes was collected");
        expect(globalThis.__seq.deref() === undefined, "sequence with a self-referencing lane was collected");
        expect(globalThis.__keptTarget.deref() !== undefined, "a reachable node keeps its connect() target alive");

        // And the kept edge still drives the graph walk: gain through it.
        const g = globalThis.__keptTarget.deref();
        g.gain.value = 0.5;
        const osc = ctx.createOscillator();
        osc.connect(globalThis.__kept);
        globalThis.__kept.gain.value = 0.5;
        osc.start();
        ctx.renderBlock(1024);
        osc.stop();
        osc.disconnect();

        // disconnect(target) drops exactly that edge.
        const other = ctx.createGain();
        globalThis.__kept.connect(other);
        globalThis.__kept.disconnect(g);
        globalThis.__other = new WeakRef(other);
        return "SUCCESS";
    )JS"));
    collectNow();
    runScript("reference cycles through nodes: disconnect releases", withPrelude(R"JS(
        expect(globalThis.__keptTarget.deref() === undefined, "a disconnected target is collectable");
        expect(globalThis.__other.deref() !== undefined, "the other edge survived disconnect(target)");
        globalThis.__kept = undefined;
        return "SUCCESS";
    )JS"));
}

static void test_midi() {
    runScript("MidiInput.injectMessage through processEvents", withPrelude(R"JS(
        const midi = ctx.createMidiInput();
        expect(midi instanceof MidiInput, "createMidiInput");
        const va = ctx.createVoiceAllocator(4);
        const setup = [];
        va.setVoiceSetup((voice, note, vel) => { churn(); setup.push(note); });
        midi.connectToAllocator(va);

        const raw = [], ccs = [], bends = [];
        midi.onRawEvent((e) => { churn(); raw.push(e.type + ":" + e.channel + ":" + e.data1 + ":" + e.data2); });
        midi.onControlChange(74, (ch, cc, v) => { churn(); ccs.push(ch + "/" + cc + "/" + v); });
        midi.onPitchBend((ch, v) => { churn(); bends.push(ch + "/" + v); });

        expect(midi.injectMessage([0x90, 60, 127]) === true, "note on accepted");
        expect(midi.injectMessage(new Uint8Array([0x91, 64, 64]), ctx.currentTime) === true, "Uint8Array accepted");
        expect(midi.injectMessage([0xB2, 74, 99]) === true, "CC accepted");
        expect(midi.injectMessage([0xE0, 0x00, 0x40]) === true, "pitch bend accepted");
        expect(midi.injectMessage([0xF8]) === false, "clock ignored");
        expect(midi.injectMessage([0x90, 60]) === false, "truncated note ignored");
        expect(va.activeVoiceCount === 0, "nothing dispatched before processEvents");

        midi.processEvents();
        expect(raw.join(",") === "noteon:0:60:127,noteon:1:64:64,controlchange:2:74:99,pitchbend:0:0:0",
               "raw events: " + raw.join(","));
        expect(ccs.join(",") === "2/74/99", "CC callback: " + ccs.join(","));
        expect(bends.join(",") === "0/0", "pitch bend callback: " + bends.join(","));
        expect(va.activeVoiceCount === 2, "allocator voices: " + va.activeVoiceCount);
        expect(setup.join(",") === "60,64", "voice setup ran for MIDI notes: " + setup.join(","));

        midi.injectMessage([0x80, 60, 0]);
        midi.injectMessage([0x91, 64, 0]);
        midi.processEvents();
        expect(va.activeVoiceCount === 0, "note offs released: " + va.activeVoiceCount);
        return "SUCCESS";
    )JS"));
}

static void test_mic_and_media() {
    runScript("bro.mic options and chunk delivery", withPrelude(R"JS(
        const chunks = [];
        bro.mic.start({
            get chunkFrames() { churn(); return 160; },
            targetRate: 0,
            live: false,
            samples: true,
            onChunk: (c) => { churn(); chunks.push(c); },
        });
        const input = new Float32Array(160 * 4);
        for (let i = 0; i < input.length; i++) input[i] = 0.5 * Math.sin(i / 5);
        bro.mic.feed(input);
        bro.mic.drain();
        bro.mic.stop();
        expect(chunks.length === 4, "chunks delivered: " + chunks.length);
        expect(chunks[0].samples instanceof Float32Array && chunks[0].samples.length === 160, "chunk samples");
        expect(chunks[1].peak > 0.3, "chunk peak " + chunks[1].peak);
        near(chunks[2].samples[7], input[2 * 160 + 7], 1e-6, "chunk samples are the fed PCM");

        // A backlog past the ring: the oldest chunks are dropped and counted,
        // the rest arrive in order with their own samples.
        const seen = [];
        bro.mic.start({ chunkFrames: 1, targetRate: 0, live: false, samples: true,
                        onChunk: (c) => { seen.push(c.index, c.samples[0]); } });
        const backlog = new Float32Array(4096 + 10);
        for (let i = 0; i < backlog.length; i++) backlog[i] = i / 8192;
        bro.mic.feed(backlog);
        bro.mic.drain();
        const st = bro.mic.stats();
        expect(st.dropped === 10 && st.chunkCount === 4106, "backlog stats " + JSON.stringify(st));
        expect(seen.length === 2 * 4096 && seen[0] === 10, "backlog delivered from chunk 10: " + seen[0]);
        near(seen[1], 10 / 8192, 1e-7, "chunk 10's own samples");
        near(seen[seen.length - 1], 4105 / 8192, 1e-7, "last chunk's own samples");

        // Stop from inside a chunk callback, then restart with another size.
        let calls = 0;
        bro.mic.start({ chunkFrames: 8, targetRate: 0, live: false, samples: true,
                        onChunk: () => { calls++; bro.mic.stop(); } });
        bro.mic.feed(new Float32Array(8 * 3));
        bro.mic.drain();
        expect(calls === 1 && !bro.mic.isActive(), "stop inside onChunk ends the batch");
        const sizes = [];
        bro.mic.start({ chunkFrames: 32, targetRate: 0, live: false, samples: true,
                        onChunk: (c) => { sizes.push(c.samples.length); } });
        bro.mic.feed(new Float32Array(32 * 2));
        bro.mic.drain();
        bro.mic.stop();
        expect(sizes.length === 2 && sizes[0] === 32, "restart with a new chunk size: " + sizes);

        expect(typeof __nativeGetUserMedia === "function", "__nativeGetUserMedia installed");
        const ms = ctx.createMediaStreamSource(new MediaStream());
        expect(ms instanceof MediaStreamAudioSourceNode, "createMediaStreamSource");
        return "SUCCESS";
    )JS"));
}

int main() {
    std::cout << "Running broaudio API GC-contract tests..." << std::endl;
    const char* stress = std::getenv("BRONZE_GC_STRESS");
    std::cout << "  (BRONZE_GC_STRESS=" << (stress ? stress : "unset") << ")" << std::endl;

    broaudio::Engine engine;
    TEST_CHECK(engine.initHeadless());
    broaudio::api::setAudioEngine(&engine);

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        broaudio::api::installMic();
        test_buffers();
        test_decode();
        test_nodes();
        test_param_ownership();
        test_cycles_collect();
        test_sequencer();
        test_midi();
        test_mic_and_media();
        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio API GC-contract tests passed!" << std::endl;
    return 0;
}
