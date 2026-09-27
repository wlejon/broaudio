// Synthesis graphs through the JS binding (src/api/host_audio_synth_graph.cpp):
// `new SynthGraph(desc)` and ctx.createSynthGraph, the params / layers
// introspection, validation errors (TypeError / RangeError, "SynthGraph: "
// and the field's path), render() determinism and its AudioBuffer being a
// clip bro.ear measures, and ctx.playSynth / releaseSynth on a headless
// engine driven by ctx.renderBlock. Registered plainly and under
// BRONZE_GC_STRESS=1 + BRONZE_GC_POISON=1 (tests/CMakeLists.txt).

#include "api.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "broaudio/engine.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace ev = bronze::embed;

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
    function expect(cond, what) { if (!cond) throw new Error(what); }
    function peak(d, from, to) {
        let p = 0;
        for (let i = from; i < to; i++) p = Math.max(p, Math.abs(d[i]));
        return p;
    }
    function same(a, b) {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (Math.abs(a[i] - b[i]) > 1e-4) return false;
        return true;
    }
    // fn() throws a `kind` whose message starts with "SynthGraph: " and
    // contains `part`.
    function throws(fn, kind, part, what) {
        let err = null;
        try { fn(); } catch (e) { err = e; }
        expect(err !== null, what + ": did not throw");
        expect(err instanceof kind, what + ": threw " + err.name + " (" + err.message + ")");
        expect(err.message.indexOf("SynthGraph: ") === 0, what + ": message " + err.message);
        expect(err.message.indexOf(part) >= 0, what + ": message " + err.message + " lacks " + part);
    }
    const bell = {
        nodes: {
            bell: { type: "fm", freq: { value: 880, jitter: 0.01 }, ratio: 3.5, index: "ie", feedback: 0.2, gain: "ae" },
            ie: { type: "env", attack: 0.001, decay: 0.4, sustain: 0, release: 0.1, peak: 6 },
            ae: { type: "env", attack: 0.002, decay: 0.5, sustain: 0, release: 0.1 }
        },
        output: "bell"
    };
    const hit = {
        layers: {
            crack: { nodes: {
                n: { type: "noise", color: "white", gain: "e" },
                e: { type: "env", attack: 0.0005, decay: { value: 0.03, jitter: 0.2 }, sustain: 0, release: 0.01 },
                hp: { type: "filter", mode: "highpass", input: "n", cutoff: 1800, q: 0.7 }
            }, output: "hp" },
            ring: { offset: 0.002, gain: 0.5, nodes: {
                x: { type: "noise", gain: "xe" },
                xe: { type: "env", attack: 0.0002, decay: 0.004, sustain: 0, release: 0.001 },
                r: { type: "resonator", input: "x", freq: 520, modes: [
                    { ratio: 1, decay: 0.3, gain: 0.5 }, { ratio: 2.76, decay: 0.2, gain: 0.3 }] }
            }, output: "r" }
        }
    };
    const pad = {
        nodes: {
            o: { type: "osc", wave: "saw", freq: 220, gain: "e" },
            e: { type: "env", attack: 0.01, decay: 0.05, sustain: 0.5, release: 0.05 }
        },
        output: "o"
    };
)JS";

static std::string withPrelude(const char* body) {
    return std::string("(function() {") + kPrelude + body + "\n})()";
}

static void test_construct() {
    runScript("construct and introspect", withPrelude(R"JS(
        const g = new SynthGraph(bell);
        expect(g instanceof SynthGraph, "instanceof");
        const g2 = ctx.createSynthGraph(JSON.stringify(bell));
        expect(g2 instanceof SynthGraph, "createSynthGraph from a JSON string");
        const names = g.paramNames;
        expect(Array.isArray(names) && names.indexOf("bell.freq") >= 0, "paramNames: " + names);
        const p = g.params["bell.freq"];
        expect(p.value === 880 && Math.abs(p.jitter - 0.01) < 1e-6 && p.jitterAbs === 0, "bell.freq info");
        expect(p.min <= 880 && p.max >= 880, "bell.freq range");
        expect(Object.keys(g.params).length === names.length, "params and paramNames agree");
        const layers = new SynthGraph(hit).layers;
        expect(layers.length === 2 && layers[0].id === "crack" && layers[1].id === "ring", "layers");
        expect(typeof layers[0].shape === "string" && layers[0].shape !== layers[1].shape, "shape keys");
        expect(g.layers[0].shape === g2.layers[0].shape, "one description, one shape");
        const pre = g.precompile();
        expect(typeof pre === "boolean" && g.compiled === pre, "precompile / compiled");
        return "SUCCESS";
    )JS"));
}

static void test_errors() {
    runScript("validation errors", withPrelude(R"JS(
        throws(() => new SynthGraph(), TypeError, "required", "no description");
        throws(() => new SynthGraph(42), TypeError, "object", "a number");
        throws(() => new SynthGraph("{nope"), TypeError, "JSON", "bad JSON");
        throws(() => new SynthGraph({ nodes: { a: { type: "swep" } }, output: "a" }), TypeError,
               "nodes.a", "unknown node kind");
        throws(() => new SynthGraph({ nodes: { a: { type: "osc", freq: -5 } }, output: "a" }), RangeError,
               "nodes.a.freq", "negative frequency");
        throws(() => new SynthGraph({ nodes: { a: { type: "osc", freq: 100 } }, output: "b" }), TypeError,
               "output", "unknown output");

        const g = new SynthGraph(bell);
        throws(() => g.render({ sampleRat: 48000 }), TypeError, "unknown option 'sampleRat'", "render key");
        throws(() => g.render({ seed: 1.5 }), RangeError, "seed", "fractional seed");
        throws(() => g.render({ seed: -1 }), RangeError, "seed", "negative seed");
        throws(() => g.render({ sampleRate: 100 }), RangeError, "sampleRate", "tiny rate");
        throws(() => g.render({ jitter: 1 }), TypeError, "jitter", "jitter not boolean");
        throws(() => g.render({ params: { "bell.frq": 1 } }), TypeError, "params.bell.frq", "unknown param");
        throws(() => g.render({ params: { "bell.freq": -1 } }), RangeError, "params.bell.freq", "param range");
        throws(() => g.render.call({}, {}), TypeError, "not a SynthGraph", "wrong this");
        throws(() => ctx.playSynth(g, { position: [1, 2] }), TypeError, "position", "short position");
        throws(() => ctx.playSynth(g, { pan: 3 }), RangeError, "pan", "pan range");
        throws(() => ctx.playSynth(g, { volume: 1 }), TypeError, "unknown option 'volume'", "play key");
        throws(() => ctx.playSynth(), TypeError, "graph is required", "no graph");
        return "SUCCESS";
    )JS"));
}

static void test_render() {
    runScript("render is deterministic and bro.ear measures it", withPrelude(R"JS(
        const g = new SynthGraph(hit);
        const a = g.render({ seed: 7 });
        expect(a instanceof AudioBuffer, "an AudioBuffer");
        expect(a.numberOfChannels === 1 && a.sampleRate === ctx.sampleRate, "mono at the context rate");
        expect(a.length > 1000 && a.length < ctx.sampleRate * 2, "ends by itself: " + a.length);
        const d = a.getChannelData(0);
        expect(peak(d, 0, d.length) > 0.01, "audible");

        const b = g.render({ seed: 7 });
        expect(same(d, b.getChannelData(0)), "same seed, same samples");
        const interp = g.render({ seed: 7, compiled: false });
        expect(same(d, interp.getChannelData(0)), "compiled == interpreted");
        const c = g.render({ seed: 8 });
        expect(!same(d, c.getChannelData(0)), "another seed differs");
        const r = g.render({ seed: 7, sampleRate: 44100 });
        expect(r.sampleRate === 44100, "explicit rate");
        const lo = g.render({ seed: 7, params: { "hp.cutoff": 400 } });
        expect(!same(d, lo.getChannelData(0)), "an override changes the sound");
        const cut = g.render({ seed: 7, maxDuration: 0.01 });
        expect(cut.length === Math.round(ctx.sampleRate * 0.01), "maxDuration cuts: " + cut.length);

        const bellBuf = new SynthGraph(bell).render({ seed: 1, jitter: false });
        const bellBuf2 = new SynthGraph(bell).render({ seed: 2, jitter: false });
        expect(same(bellBuf.getChannelData(0), bellBuf2.getChannelData(0)), "no jitter, no noise: seed-free");

        if (typeof bro !== "undefined" && bro.ear) {
            const m = bro.ear.measure(bellBuf);
            expect(m.sampleRate === ctx.sampleRate && m.duration > 0.3, "ear.measure of a render");
            expect(m.partials.length > 0, "the bell has partials");
            const cmp = bro.ear.compare(b, a);
            expect(cmp.score === 0, "a render compared to itself (0 = identical): " + cmp.score);
            const other = bro.ear.compare(c, a);
            expect(other.score > 0, "another seed is a distance away: " + other.score);
        }
        return "SUCCESS";
    )JS"));
}

static void test_play() {
    runScript("playSynth plays, schedules and releases", withPrelude(R"JS(
        ctx.renderBlock(1024);
        const g = new SynthGraph(hit);
        const id = ctx.playSynth(g, { seed: 3, gain: 0.8 });
        expect(typeof id === "number" && id > 0, "an id: " + id);
        let out = ctx.renderBlock(4096);
        expect(peak(out, 0, 4096) > 1e-3, "audible: " + peak(out, 0, 4096));
        for (let k = 0; k < 30; k++) out = ctx.renderBlock(4096);   // 2.5 s: long gone
        expect(peak(out, 0, 4096) < 1e-5, "ends by itself");
        expect(!ctx.isClipPlaying(id), "finished playback is not playing");

        const later = ctx.playSynth(new SynthGraph(bell), { when: ctx.currentTime + 0.2, pan: -1 });
        out = ctx.renderBlock(4096);
        expect(peak(out, 0, 4096) < 1e-6, "scheduled in the future is silent now");
        for (let k = 0; k < 3; k++) out = ctx.renderBlock(4096);
        let p = 0;
        for (let k = 0; k < 3; k++) p = Math.max(p, peak(ctx.renderBlock(4096), 0, 4096));
        expect(p > 1e-3, "plays once the clock reaches when: " + p);

        const held = ctx.playSynth(pad, { position: [2, 0, -1] });   // a bare description
        ctx.renderBlock(4096);
        for (let k = 0; k < 4; k++) out = ctx.renderBlock(4096);
        expect(peak(out, 0, 4096) > 1e-3, "sustaining");
        ctx.releaseSynth(held);
        for (let k = 0; k < 6; k++) out = ctx.renderBlock(4096);
        expect(peak(out, 0, 4096) < 1e-5, "released and gone");
        ctx.releaseSynth(123456);   // unknown ids are ignored

        const stopped = ctx.playSynth(pad, {});
        ctx.renderBlock(4096);
        ctx.stopClip(stopped);
        ctx.renderBlock(4096);
        expect(peak(ctx.renderBlock(4096), 0, 4096) < 1e-5, "stopClip stops a synth playback");
        return "SUCCESS";
    )JS"));
}

int main() {
    std::cout << "Running broaudio API synth-graph tests..." << std::endl;
    const char* stress = std::getenv("BRONZE_GC_STRESS");
    std::cout << "  (BRONZE_GC_STRESS=" << (stress ? stress : "unset") << ")" << std::endl;

    broaudio::Engine engine;
    TEST_CHECK(engine.initHeadless());
    broaudio::api::setAudioEngine(&engine);

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        broaudio::api::installEar();
        test_construct();
        test_errors();
        test_render();
        test_play();
        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio API synth-graph tests passed!" << std::endl;
    return 0;
}
