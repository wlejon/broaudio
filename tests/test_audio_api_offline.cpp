// OfflineAudioContext (host_audio_offline*.cpp): the rendered samples of
// small graphs against Chromium's for the same graphs (the numbers below
// were read from Chromium rendering the same scripts), the promise /
// `complete` / `statechange` order, suspend(time) and resume(), and
// argument validation. No engine voice is touched: the context renders
// in-process with the live engine idle.

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

static const char* kPrelude = R"JS(
    globalThis.near = function (a, b, eps, what) {
        if (!(Math.abs(a - b) <= eps)) throw new Error(what + ": " + a + " != " + b);
    };
    globalThis.expect = function (cond, what) { if (!cond) throw new Error(what); };
    globalThis.rms = function (d) { let s = 0; for (let i = 0; i < d.length; i++) s += d[i] * d[i]; return Math.sqrt(s / d.length); };
    globalThis.RESULT = 'pending';
    globalThis.render = function (build, ch, len) {
        const c = new OfflineAudioContext(ch || 1, len || 4410, 44100);
        build(c);
        return c.startRendering();
    };
)JS";

// Run `body` (an async function's body), drain the jobs, and expect
// RESULT === 'SUCCESS'.
static void runAsync(const char* label, const std::string& body) {
    std::cout << "  " << label << "..." << std::endl;
    std::string script = "RESULT = 'pending'; (async () => {" + body +
                         "})().then(() => { RESULT = 'SUCCESS'; }, (e) => { RESULT = String(e && e.stack || e); });";
    ev::CallResult res = bronze::eval::evalScript(script);
    if (res.thrown) {
        std::cerr << label << " threw: " << ev::toUtf8(res.value) << std::endl;
        std::exit(1);
    }
    for (int i = 0; i < 64 && ev::microtasksPending(); ++i) ev::drainMicrotasks();
    ev::CallResult r = bronze::eval::evalScript("RESULT");
    if (ev::toUtf8(r.value) != "SUCCESS") {
        std::cerr << label << ": " << ev::toUtf8(r.value) << std::endl;
        std::exit(1);
    }
}

static void test_samples_match_chromium() {
    // oscillator -> gain(0.5) -> destination.
    runAsync("sine through a gain", R"JS(
        const b = await render((c) => {
            const o = c.createOscillator(); const g = c.createGain(); g.gain.value = 0.5;
            o.connect(g).connect(c.destination); o.start();
        });
        const d = b.getChannelData(0);
        expect(b.length === 4410 && b.sampleRate === 44100 && b.numberOfChannels === 1, 'buffer shape');
        near(d[0], 0, 1e-6, 'd[0]'); near(d[1], 0.031324, 1e-5, 'd[1]'); near(d[2], 0.062525, 1e-5, 'd[2]');
        near(d[3], 0.093481, 1e-5, 'd[3]'); near(d[1000], -0.070998, 1e-5, 'd[1000]');
        near(rms(d), 0.353553, 1e-5, 'rms');
    )JS");
    // Band-limited built-in waves (Chromium's tables).
    runAsync("sawtooth and square", R"JS(
        let d = (await render((c) => { const o = c.createOscillator(); o.type = 'sawtooth'; o.frequency.value = 220; o.connect(c.destination); o.start(); })).getChannelData(0);
        near(d[1], 0.006418, 1e-5, 'saw d[1]'); near(d[3], 0.022421, 1e-5, 'saw d[3]'); near(rms(d), 0.487996, 1e-5, 'saw rms');
        d = (await render((c) => { const o = c.createOscillator(); o.type = 'square'; o.frequency.value = 220; o.connect(c.destination); o.start(); })).getChannelData(0);
        near(d[1], 0.955512, 1e-5, 'square d[1]'); near(rms(d), 0.845973, 1e-5, 'square rms');
    )JS");
    runAsync("lowpass biquad", R"JS(
        const d = (await render((c) => { const o = c.createOscillator(); o.type = 'sawtooth'; const f = c.createBiquadFilter(); f.frequency.value = 800; f.Q.value = 3; o.connect(f).connect(c.destination); o.start(); })).getChannelData(0);
        near(d[3], 0.000933, 1e-5, 'd[3]'); near(d[1000], -0.140841, 1e-5, 'd[1000]'); near(rms(d), 0.537659, 1e-5, 'rms');
    )JS");
    runAsync("automation and start/stop times", R"JS(
        const d = (await render((c) => { const o = c.createOscillator(); const g = c.createGain(); g.gain.setValueAtTime(0, 0); g.gain.linearRampToValueAtTime(1, 0.05); g.gain.setTargetAtTime(0.2, 0.06, 0.01); o.connect(g).connect(c.destination); o.start(0.01); o.stop(0.09); })).getChannelData(0);
        near(d[1000], -0.211770, 1e-5, 'd[1000]'); near(rms(d), 0.412028, 1e-5, 'rms');
    )JS");
    // A feedback loop through a DelayNode: the echo comes a quantum after
    // the delay, as in Chromium.
    runAsync("feedback through a delay", R"JS(
        const d = (await render((c) => { const o = c.createOscillator(); const dl = c.createDelay(1); dl.delayTime.value = 0.01; const fb = c.createGain(); fb.gain.value = 0.5; o.connect(dl); dl.connect(fb); fb.connect(dl); dl.connect(c.destination); o.start(); o.stop(0.005); })).getChannelData(0);
        near(d[442], 0.062648, 1e-5, 'direct'); near(d[900], 0, 1e-6, 'gap'); near(d[1100], -0.299055, 1e-5, 'echo');
        near(d[1300], 0, 1e-6, 'after the echo'); near(rms(d), 0.180616, 1e-5, 'rms');
    )JS");
    runAsync("audio-rate frequency modulation", R"JS(
        const d = (await render((c) => { const m = c.createOscillator(); m.frequency.value = 50; const mg = c.createGain(); mg.gain.value = 100; const o = c.createOscillator(); m.connect(mg).connect(o.frequency); o.connect(c.destination); m.start(); o.start(); })).getChannelData(0);
        near(d[100], 0.450634, 1e-5, 'd[100]'); near(rms(d), 0.707107, 1e-5, 'rms');
    )JS");
    runAsync("looping buffer source at another rate", R"JS(
        const d = (await render((c) => { const b = c.createBuffer(1, 1000, 22050); const x = b.getChannelData(0); for (let i = 0; i < 1000; i++) x[i] = Math.sin(i / 10); const s = c.createBufferSource(); s.buffer = b; s.loop = true; s.playbackRate.value = 1.5; s.connect(c.destination); s.start(0.001); })).getChannelData(0);
        near(d[100], -0.867578, 1e-5, 'd[100]'); near(d[1000], 0.534488, 1e-5, 'd[1000]'); near(rms(d), 0.704740, 1e-5, 'rms');
    )JS");
    runAsync("stereo panner, wave shaper, constant source", R"JS(
        let b = await render((c) => { const o = c.createOscillator(); const p = c.createStereoPanner(); p.pan.value = 0.3; o.connect(p).connect(c.destination); o.start(); }, 2);
        near(rms(b.getChannelData(0)), 0.369462, 1e-5, 'pan L'); near(rms(b.getChannelData(1)), 0.602907, 1e-5, 'pan R');
        let d = (await render((c) => { const o = c.createOscillator(); const w = c.createWaveShaper(); w.curve = new Float32Array([-1, -0.2, 0, 0.5, 1]); o.connect(w).connect(c.destination); o.start(); })).getChannelData(0);
        near(rms(d), 0.667924, 1e-5, 'shaper');
        d = (await render((c) => { const k = c.createConstantSource(); k.offset.value = 0.25; const o = c.createOscillator(); const g = c.createGain(); g.gain.value = 0; k.connect(g.gain); o.connect(g).connect(c.destination); k.start(); o.start(); })).getChannelData(0);
        near(rms(d), 0.176777, 1e-5, 'constant source into a gain');
    )JS");
}

static void test_events_and_suspend() {
    runAsync("promise, then complete, statechange", R"JS(
        const c = new OfflineAudioContext({ numberOfChannels: 2, length: 1000, sampleRate: 22050 });
        expect(c.state === 'suspended' && c.length === 1000 && c.sampleRate === 22050, 'initial');
        expect(c.destination.channelCount === 2 && c.destination.maxChannelCount === 2, 'destination channels');
        const log = [];
        c.onstatechange = () => log.push('state:' + c.state);
        c.oncomplete = (e) => log.push('complete:' + (e instanceof OfflineAudioCompletionEvent) + ':' + e.renderedBuffer.length);
        const p = c.startRendering();
        expect(c.state === 'running', 'running at once');
        const b = await p;
        log.push('resolved:' + b.length);
        for (let i = 0; i < 8; i++) await null;
        expect(log[0] === 'state:running', 'statechange first: ' + log);
        expect(log.indexOf('resolved:1000') >= 0, 'resolved: ' + log);
        expect(log.indexOf('complete:true:1000') > log.indexOf('resolved:1000'), 'complete after the promise: ' + log);
        expect(c.state === 'closed', 'closed');
        let threw = false;
        try { await c.startRendering(); } catch (e) { threw = e.name === 'InvalidStateError'; }
        expect(threw, 'startRendering twice rejects');
    )JS");
    runAsync("suspend at a time, change the graph, resume", R"JS(
        const c = new OfflineAudioContext(1, 4410, 44100);
        const o = c.createOscillator(); const g = c.createGain(); o.connect(g).connect(c.destination); o.start();
        c.suspend(0.05).then(() => {
            near(c.currentTime, 2304 / 44100, 1e-9, 'suspended at the quantum boundary');
            expect(c.state === 'suspended', 'suspended');
            g.gain.value = 0.1; c.resume();
        });
        const d = (await c.startRendering()).getChannelData(0);
        near(d[1000], -0.141996, 1e-5, 'before'); near(rms(d), 0.513564, 1e-5, 'rms');
        let after = 0; for (let i = 2304; i < 4410; i++) after = Math.max(after, Math.abs(d[i]));
        near(after, 0.1, 1e-3, 'after the suspend the gain is 0.1');
    )JS");
}

static void test_validation() {
    runAsync("constructor and node validation", R"JS(
        const bad = (fn, name) => { let e = null; try { fn(); } catch (x) { e = x; } expect(e && e.name === name, 'expected ' + name + ', got ' + e); };
        bad(() => new OfflineAudioContext(0, 100, 44100), 'NotSupportedError');
        bad(() => new OfflineAudioContext(1, 0, 44100), 'NotSupportedError');
        bad(() => new OfflineAudioContext(1, 100, 100), 'NotSupportedError');
        const c = new OfflineAudioContext(1, 128, 44100);
        bad(() => c.createIIRFilter([0, 0], [1]), 'InvalidStateError');
        bad(() => c.createIIRFilter([1], [0]), 'InvalidStateError');
        bad(() => c.createGain().channelCount = 0, 'NotSupportedError');
        const k = c.createConstantSource(); k.start();
        bad(() => k.start(), 'InvalidStateError');
        expect(c.createConstantSource() instanceof AudioNode, 'ConstantSourceNode is an AudioNode');
        let rejected = false;
        try { await c.suspend(1); } catch (e) { rejected = e.name === 'InvalidStateError'; }
        expect(rejected, 'suspend past the length rejects');
    )JS");
}

int main() {
    std::cout << "Running broaudio OfflineAudioContext tests..." << std::endl;
    broaudio::Engine engine;
    TEST_CHECK(engine.initHeadless());
    broaudio::api::setAudioEngine(&engine);

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        ev::CallResult res = bronze::eval::evalScript(kPrelude);
        TEST_CHECK(!res.thrown);
        test_samples_match_chromium();
        test_events_and_suspend();
        test_validation();
        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio OfflineAudioContext tests passed!" << std::endl;
    return 0;
}
