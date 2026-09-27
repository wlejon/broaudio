// bro.ear.fit through the JS binding (src/api/host_audio_ear_fit*.cpp):
// recovering hidden parameters of a SynthGraph from its own render, the
// result's shape and its `render` options reproducing the clip bit for bit,
// measurement targets, held parameters, determinism across thread counts, a
// JS scorer and a duck-typed CLAP model scoring whole generations on the JS
// thread, onProgress (and cancelling from it), the async form (onDone,
// handle.wait(), cancel(), delivery from tickAsyncJobs) and the option
// errors. Registered plainly and under BRONZE_GC_STRESS=1 +
// BRONZE_GC_POISON=1 (tests/CMakeLists.txt).

#include "api.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "broaudio/engine.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace ev = bronze::embed;

#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::exit(1); \
    } \
} while (0)

static std::string evalString(const std::string& script, const char* label) {
    ev::CallResult res = bronze::eval::evalScript(script);
    if (res.thrown) {
        std::cerr << label << " threw: " << ev::toUtf8(res.value) << std::endl;
        std::exit(1);
    }
    return ev::toUtf8(res.value);
}

static void runScript(const char* label, const std::string& script) {
    std::cout << "  " << label << "..." << std::endl;
    const std::string r = evalString(script, label);
    if (r.rfind("SUCCESS", 0) != 0) {
        std::cerr << label << " returned: " << r << std::endl;
        std::exit(1);
    }
    if (r.size() > 7) std::cout << "   " << r.substr(7) << std::endl;
}

static const char* kPrelude = R"JS(
    function expect(cond, what) { if (!cond) throw new Error(what); }
    function same(a, b) {
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
        return true;
    }
    function throws(fn, kind, part, what) {
        let err = null;
        try { fn(); } catch (e) { err = e; }
        expect(err !== null, what + ": did not throw");
        expect(err instanceof kind, what + ": threw " + err.name + " (" + err.message + ")");
        expect(err.message.indexOf(part) >= 0, what + ": message " + err.message + " lacks " + part);
    }
    const pluck = {
        nodes: {
            o:  { type: "fm", freq: 440, ratio: 2, index: "ie", gain: "ae" },
            ie: { type: "env", attack: 0.001, decay: 0.15, sustain: 0, release: 0.05, peak: 3 },
            ae: { type: "env", attack: 0.002, decay: 0.3, sustain: 0, release: 0.05 }
        },
        output: "o", duration: 0.4
    };
    const rate = 16000;
    const hidden = { "o.freq": 587, "ae.decay": 0.12 };
)JS";

static std::string withPrelude(const char* body) {
    return std::string("(function() {") + kPrelude + body + "\n})()";
}

static void test_recover() {
    runScript("fit recovers hidden parameters from a reference", withPrelude(R"JS(
        const g = new SynthGraph(pluck);
        const ref = g.render({ params: hidden, jitter: false, sampleRate: rate });
        const r = bro.ear.fit(g, { params: ["o.freq", { name: "ae.decay" }], reference: ref,
                                   maxEvaluations: 500, stopAt: 1e-3, seed: 3 });
        expect(typeof r.distance === "number" && r.distance < 0.02, "distance " + r.distance);
        expect(Math.abs(r.params["o.freq"] / 587 - 1) < 0.01, "o.freq " + r.params["o.freq"]);
        expect(Math.abs(r.params["ae.decay"] / 0.12 - 1) < 0.1, "ae.decay " + r.params["ae.decay"]);
        expect(r.searched.length === 2 && r.searched[0].name === "o.freq" && r.searched[0].scale === "log",
               "searched");
        expect(r.searched[0].min === 110 && r.searched[0].max === 1760 && r.searched[0].start === 440, "range");
        expect(r.evaluations <= 500 && r.generations === r.history.length, "counts");
        expect(r.history[r.history.length - 1].best === r.distance, "history ends at the result");
        expect(["stopAt", "evaluations", "converged"].indexOf(r.stop) >= 0, "stop " + r.stop);
        expect(r.clip.samples instanceof Float32Array && r.clip.sampleRate === rate, "clip");
        expect(r.render.sampleRate === rate && r.render.jitter === false && r.render.seed === 1, "render opts");
        const again = g.render(r.render);
        expect(same(again.getChannelData(0), r.clip.samples), "render(result.render) reproduces the clip");
        expect(Math.abs(r.terms.reference - bro.ear.compare(r.clip, ref).score) < 1e-12, "terms.reference");
        expect(r.terms.comparison.score === r.terms.reference, "terms.comparison");
        expect(r.terms.external === null, "no external term");
        // A bare description works as well as a SynthGraph.
        const d = bro.ear.fit(pluck, { params: { "o.freq": { min: 400, max: 800, scale: "linear" } },
                                       reference: ref, maxEvaluations: 40 });
        expect(d.searched[0].scale === "linear" && d.params["o.freq"] >= 400 && d.params["o.freq"] <= 800, "desc");
        return "SUCCESS " + r.evaluations + " evaluations, distance " + r.distance.toFixed(5) + ", o.freq " +
               r.params["o.freq"].toFixed(2) + ", " + r.seconds.toFixed(2) + " s";
    )JS"));
}

static void test_measures_fixed_determinism() {
    runScript("measures, fixed parameters and thread-count determinism", withPrelude(R"JS(
        const g = new SynthGraph(pluck);
        const opts = { params: ["o.freq", "ae.decay"], fixed: { "ie.decay": 0.05 },
                       measures: { centroidHz: 900, tailTime: { value: 0.2, weight: 2 } },
                       sampleRate: rate, maxEvaluations: 300, seed: 9, threads: 1 };
        const a = bro.ear.fit(g, opts);
        expect(a.params["ie.decay"] === Math.fround(0.05), "held: " + a.params["ie.decay"]);
        expect(Object.keys(a.params).length === 3, "params = searched + fixed");
        const c = a.terms.measures.centroidHz, t = a.terms.measures.tailTime;
        expect(c.target === 900 && Math.abs(c.measured / 900 - 1) < 0.05, "centroid " + c.measured);
        expect(Math.abs(t.measured / 0.2 - 1) < 0.15, "tail " + t.measured);
        opts.threads = 5;
        const b = bro.ear.fit(g, opts);
        expect(b.distance === a.distance && b.params["o.freq"] === a.params["o.freq"], "threads 1 vs 5");
        expect(same(a.clip.samples, b.clip.samples), "same clip");
        throws(() => bro.ear.fit(g, { params: ["o.freq"], fixed: ["o.freq"], measures: { centroidHz: 1 } }),
               TypeError, "both searched and fixed", "searched and fixed");
        return "SUCCESS centroid " + c.measured.toFixed(1) + " Hz, tail " + t.measured.toFixed(3) + " s";
    )JS"));
}

static void test_scorers() {
    runScript("a JS scorer and a CLAP-shaped model score whole generations", withPrelude(R"JS(
        const g = new SynthGraph(pluck);
        let calls = 0, sizes = [];
        const scorer = (clips) => {
            calls++;
            sizes.push(clips.length);
            return clips.map(c => {
                expect(c.samples instanceof Float32Array && c.sampleRate === 8000, "scorer clip");
                const m = bro.ear.measure(c, { maxPartials: 1 });
                return m.partials.length ? Math.abs(Math.log2(m.partials[0].freqHz / 330)) : 5;
            });
        };
        const r = bro.ear.fit(g, { params: ["o.freq"], scorer, population: 6, seeds: 2, maxEvaluations: 60,
                                   sampleRate: 8000 });
        expect(calls === r.generations && sizes.every(n => n === 12), "batches " + sizes);
        expect(r.terms.external === r.distance, "external term");

        // Anything with score() / embedText() is a CLAP model to fit.
        let embedded = [], scored = 0;
        const clap = {
            embedText(s) { embedded.push(s); const e = new Float32Array(4); e[0] = s.length; return e; },
            score(clip, prompts, o) {
                scored++;
                expect(prompts.length === 2 && prompts[0] instanceof Float32Array, "cached embeddings");
                expect(prompts[1] instanceof Float32Array && prompts[1][0] === 7, "given embedding kept");
                expect(o && o.pad === "silence", "options passed");
                const m = bro.ear.measure(clip, { maxPartials: 1 });
                const p = m.partials.length ? Math.exp(-Math.abs(Math.log2(m.partials[0].freqHz / 500))) : 0;
                return { scores: new Float32Array([p, 1 - p]) };
            }
        };
        const given = new Float32Array(4); given[0] = 7;
        const c = bro.ear.fit(g, { params: ["o.freq"], clap: { model: clap, prompts: ["a bell", given],
                                   options: { pad: "silence" } }, weights: { clap: 2 },
                                   population: 6, maxEvaluations: 36, sampleRate: 8000 });
        expect(embedded.length === 1 && embedded[0] === "a bell", "embedText once per string prompt");
        expect(scored === c.evaluations, "one score() per clip");
        expect(c.distance <= 2 && c.terms.external === c.distance, "clap distance");

        throws(() => bro.ear.fit(g, { params: ["o.freq"], scorer: () => [1], maxEvaluations: 12,
                                      sampleRate: 8000 }), TypeError, "one number per clip", "short return");
        const boom = new Error("boom");
        let err = null;
        try { bro.ear.fit(g, { params: ["o.freq"], scorer: () => { throw boom; }, maxEvaluations: 12,
                               sampleRate: 8000 }); } catch (e) { err = e; }
        expect(err === boom, "the scorer's throw is rethrown as is");
        return "SUCCESS scorer 330 Hz -> o.freq " + r.params["o.freq"].toFixed(1) + " in " + r.evaluations;
    )JS"));
}

static void test_progress() {
    runScript("onProgress reports and cancels", withPrelude(R"JS(
        const g = new SynthGraph(pluck);
        const ref = g.render({ params: hidden, jitter: false, sampleRate: rate });
        const seen = [];
        const r = bro.ear.fit(g, { params: ["o.freq", "ae.decay"], reference: ref, maxEvaluations: 100000,
            onProgress(p) {
                seen.push(p);
                expect(typeof p.params["o.freq"] === "number" && p.evaluations > 0, "progress shape");
                return p.generation < 4;
            } });
        expect(r.stop === "cancelled" && r.generations === 4 && seen.length === 4, "cancel at 4: " + r.stop);
        expect(seen[3].best === r.distance, "last progress = result");
        const thrower = new Error("progress");
        let err = null;
        try { bro.ear.fit(g, { params: ["o.freq"], reference: ref, onProgress() { throw thrower; } }); }
        catch (e) { err = e; }
        expect(err === thrower, "onProgress's throw is rethrown");
        return "SUCCESS";
    )JS"));
}

static void test_async() {
    runScript("async: wait(), cancel() and a scorer served from wait()", withPrelude(R"JS(
        const g = new SynthGraph(pluck);
        const ref = g.render({ params: hidden, jitter: false, sampleRate: rate });
        let done = null, progress = 0;
        const h = bro.ear.fit(g, { params: ["o.freq", "ae.decay"], reference: ref, maxEvaluations: 200,
                                   onProgress() { progress++; }, onDone(r, info) { done = { r, info }; } });
        expect(typeof h.wait === "function" && typeof h.cancel === "function", "handle");
        expect(h.done === false && done === null, "not settled before wait / tick");
        h.wait();
        expect(h.done && done && done.info.cancelled === false && done.info.error === undefined, "settled");
        expect(done.r.evaluations > 0 && done.r.distance < 0.5 && progress > 0, "result + progress");
        const sync = bro.ear.fit(g, { params: ["o.freq", "ae.decay"], reference: ref, maxEvaluations: 200 });
        expect(sync.distance === done.r.distance && sync.params["o.freq"] === done.r.params["o.freq"],
               "async == sync");

        let scored = 0, sd = null;
        const hs = bro.ear.fit(g, { params: ["o.freq"], population: 6, maxEvaluations: 24, sampleRate: 8000,
                                    scorer: cs => { scored += cs.length; return cs.map(() => 1); },
                                    onDone(r, info) { sd = { r, info }; } });
        hs.wait();
        expect(scored === 24 && sd.r.evaluations === 24, "scorer served inside wait(): " + scored);

        let cd = null;
        const hc = bro.ear.fit(g, { params: ["o.freq"], population: 6, maxEvaluations: 1000000, sampleRate: 8000,
                                    scorer: cs => cs.map(() => 1), onDone(r, info) { cd = { r, info }; } });
        hc.cancel();
        expect(hc.cancelled, "cancelled flag");
        hc.wait();
        expect(cd && cd.info.cancelled === true && cd.r.stop === "cancelled", "onDone after cancel: " +
               (cd && cd.r && cd.r.stop));
        hc.wait();   // settled: returns at once

        // Bad options throw at the call, not in onDone.
        throws(() => bro.ear.fit(g, { params: ["nope"], reference: ref, onDone() {} }), TypeError,
               "params.nope", "async validation");
        // Left for tickAsyncJobs to deliver (the C++ side pumps it).
        globalThis.__tickDone = null;
        globalThis.__tickHandle = bro.ear.fit(g, { params: ["o.freq"], population: 6, maxEvaluations: 18,
            sampleRate: 8000, scorer: cs => cs.map(() => 0.5),
            onDone(r, info) { globalThis.__tickDone = r.evaluations; } });
        return "SUCCESS";
    )JS"));
}

static void test_errors() {
    runScript("option errors", withPrelude(R"JS(
        const g = new SynthGraph(pluck);
        const ref = g.render({ sampleRate: rate });
        throws(() => bro.ear.fit(g), TypeError, "graph and options", "no options");
        throws(() => bro.ear.fit(g, { params: ["o.freq"] }), TypeError, "a target is required", "no target");
        throws(() => bro.ear.fit(g, { reference: ref, maxEvals: 3 }), TypeError, "unknown option 'maxEvals'",
               "unknown option");
        throws(() => bro.ear.fit(g, { reference: ref, params: ["o.frq"] }), TypeError,
               "bro.ear.fit: params.o.frq: unknown parameter", "unknown param");
        throws(() => bro.ear.fit(g, { reference: ref, params: { "o.freq": { min: 900, max: 100 } } }), RangeError,
               "params.o.freq: the range", "inverted range");
        throws(() => bro.ear.fit(g, { reference: ref, params: { "o.freq": { scale: "cubic" } } }), TypeError,
               "params.o.freq.scale", "bad scale");
        throws(() => bro.ear.fit(g, { measures: { loudness: -20 } }), TypeError, "measures.loudness", "measure");
        throws(() => bro.ear.fit(g, { reference: ref, compare: { weights: { env: 1 } } }), TypeError,
               "compare.weights: unknown option 'env'", "compare weights");
        throws(() => bro.ear.fit(g, { reference: ref, seeds: 0 }), RangeError, "seeds", "seeds");
        throws(() => bro.ear.fit(g, { reference: 42 }), TypeError, "reference", "reference");
        throws(() => bro.ear.fit(g, { clap: { model: {}, prompts: ["x"] } }), TypeError, "clap.model", "clap");
        throws(() => bro.ear.fit({ nodes: {} }, { reference: ref }), TypeError, "SynthGraph: ", "bad graph");
        return "SUCCESS";
    )JS"));
}

int main() {
    std::cout << "Running broaudio API ear.fit tests..." << std::endl;
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
        test_recover();
        test_measures_fixed_determinism();
        test_scorers();
        test_progress();
        test_async();
        test_errors();

        // The job test_async left running settles through the host's tick.
        std::cout << "  async delivery from tickAsyncJobs..." << std::endl;
        const auto t0 = std::chrono::steady_clock::now();
        std::string got = "null";
        while (got == "null" && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) {
            broaudio::api::tickAsyncJobs();
            got = evalString("String(globalThis.__tickDone)", "tick poll");
            if (got == "null") std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        TEST_CHECK(got == "18");
        TEST_CHECK(evalString("String(globalThis.__tickHandle.done)", "tick handle") == "true");

        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio API ear.fit tests passed!" << std::endl;
    return 0;
}
