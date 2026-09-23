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
        osc.stop();
        comp.disconnect();
        expect(!ctx.getBusCompressorEnabled(0), "disconnect disables it");
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
        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio API contract tests passed!" << std::endl;
    return 0;
}
