// Live params through the JS binding: AudioParam value sets and scheduled
// automation reaching sources that are already playing, the BiquadFilter's
// detune moving the running filter, AudioBufferSourceNode loop points /
// start offset / duration / onended, and isClipPlaying. Registered plainly
// and under BRONZE_GC_STRESS=1 + BRONZE_GC_POISON=1 (tests/CMakeLists.txt):
// onended runs JS from inside the binding, and a playing source is held by
// the binding rather than by the script.
//
// Runs on its own headless broaudio::Engine; ctx.renderBlock() drives it and
// returns the mono mixdown, which the checks measure (peaks, and pitch from
// upward zero crossings).

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
    function churn() { const junk = []; for (let i = 0; i < 64; i++) junk.push({ i: i, s: "x" + i }); return junk.length; }
    function peak(out, from, to) {
        let p = 0;
        for (let i = from; i < to; i++) p = Math.max(p, Math.abs(out[i]));
        return p;
    }
    // Pitch from upward zero crossings over [from, to).
    function pitch(out, from, to) {
        let n = 0;
        for (let i = from + 1; i < to; i++) if (out[i - 1] < 0 && out[i] >= 0) n++;
        return n * sr / (to - from);
    }
    function nearPitch(hz, want, what) { near(hz, want, want * 0.06 + 2, what); }
    // A mono buffer at `rate` holding a `hz` sine from `from` to `to` (frames).
    function sineBuffer(frames, rate, hz, from, to) {
        const b = ctx.createBuffer(1, frames, rate);
        const d = b.getChannelData(0);
        for (let i = from; i < to; i++) d[i] = 0.5 * Math.sin(2 * Math.PI * hz * (i - from) / rate);
        return b;
    }
    function settle() { ctx.renderBlock(4096); ctx.renderBlock(4096); }
    // The output peak of an unscaled 0.5-amplitude buffer: the yardstick for
    // "sounding" and "silent" (the master chain scales what renderBlock sees).
    function refLevel() {
        const s = ctx.createBufferSource();
        s.buffer = sineBuffer(sr, sr, 441, 0, sr);
        s.loop = true;
        s.connect(ctx.destination);
        s.start();
        const p = peak(ctx.renderBlock(4096), 1024, 4096);
        s.stop();
        ctx.renderBlock(1024);
        expect(p > 1e-4, "reference buffer audible: " + p);
        return p;
    }
)JS";

static std::string withPrelude(const char* body) {
    return std::string("(function() {") + kPrelude + body + "\n})()";
}

static void test_oscillator_live() {
    runScript("value sets reach a playing oscillator", withPrelude(R"JS(
        const osc = ctx.createOscillator();
        const g = ctx.createGain();
        g.gain.value = 0.4;
        osc.connect(g).connect(ctx.destination);
        osc.start();
        let out = ctx.renderBlock(4096);
        const p1 = peak(out, 2048, 4096);
        expect(p1 > 1e-4, "oscillator audible: " + p1);
        nearPitch(pitch(out, 1024, 4096), 440, "440 Hz before any change");

        g.gain.value = 0.1;                       // a GainNode on the path
        out = ctx.renderBlock(4096);
        near(peak(out, 2048, 4096) / p1, 0.25, 0.03, "path gain set after start");

        osc.gain.value = 0.5;                     // the voice's own gain multiplies
        out = ctx.renderBlock(4096);
        near(peak(out, 2048, 4096) / p1, 0.125, 0.02, "own gain x path gain");
        osc.gain.value = 1;

        osc.frequency.value = 880;
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 1024, 4096), 880, "frequency set after start");

        osc.frequency.value = 440;
        osc.detune.value = 1200;
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 1024, 4096), 880, "detune +1200 after start");
        osc.detune.value = -1200;
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 1024, 4096), 220, "detune -1200 after start");

        osc.stop();
        settle();
        return "SUCCESS";
    )JS"));

    runScript("scheduled automation reaches a playing oscillator", withPrelude(R"JS(
        const osc = ctx.createOscillator();
        const g = ctx.createGain();
        g.gain.value = 0.5;
        osc.connect(g).connect(ctx.destination);
        osc.start();
        let out = ctx.renderBlock(4096);
        const full = peak(out, 2048, 4096);
        expect(full > 1e-4, "audible");

        // A linear ramp to silence over the next 4096 frames moves within
        // the block (renderBlock evaluates automation per 128-frame quantum).
        const t0 = ctx.currentTime;
        g.gain.setValueAtTime(0.5, t0);
        g.gain.linearRampToValueAtTime(0, t0 + 4096 / sr);
        out = ctx.renderBlock(4096);
        const early = peak(out, 256, 1024), late = peak(out, 3584, 4096);
        expect(early > full * 0.6, "ramp start: " + early + " vs " + full);
        // Ideal is 1/8 of full at 3584; the engine's gain smoother lags it.
        expect(late < full * 0.3, "ramp end: " + late + " vs " + full);
        const mid = peak(out, 1920, 2176);
        expect(mid < early * 0.8 && mid > late, "ramp midway: " + early + " > " + mid + " > " + late);
        out = ctx.renderBlock(4096);
        expect(peak(out, 1024, 4096) < full * 0.02, "ramp held at 0");
        near(g.gain.value, 0, 1e-6, "value reads the automated value");

        g.gain.cancelScheduledValues(0);
        g.gain.value = 0.5;
        // A step scheduled in the future lands when the clock gets there.
        osc.frequency.setValueAtTime(880, ctx.currentTime + 4096 / sr);
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 1024, 4096), 440, "before the scheduled step");
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 1024, 4096), 880, "after the scheduled step");

        // A ramp on detune, one octave down over one block.
        osc.frequency.cancelScheduledValues(0);
        osc.frequency.value = 880;
        const t1 = ctx.currentTime;
        osc.detune.setValueAtTime(0, t1);
        osc.detune.linearRampToValueAtTime(-1200, t1 + 4096 / sr);
        ctx.renderBlock(4096);
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 1024, 4096), 440, "detune ramp arrived");

        osc.stop();
        settle();
        return "SUCCESS";
    )JS"));
}

static void test_biquad_detune() {
    runScript("BiquadFilter detune moves the running filter", withPrelude(R"JS(
        const osc = ctx.createOscillator();
        osc.frequency.value = 2000;
        const f = ctx.createBiquadFilter();
        f.frequency.value = 500;
        osc.connect(f).connect(ctx.destination);
        osc.start();
        let out = ctx.renderBlock(4096);
        const low = peak(out, 2048, 4096);

        // 500 Hz two octaves up is a 2 kHz cutoff: the tone comes through.
        f.detune.value = 2400;
        out = ctx.renderBlock(4096);
        const open = peak(out, 2048, 4096);
        expect(open > low * 4, "detune opened the filter: " + low + " -> " + open);

        // The same cutoff reached through frequency alone sounds the same.
        f.detune.value = 0;
        f.frequency.value = 2000;
        out = ctx.renderBlock(4096);
        near(peak(out, 2048, 4096), open, open * 0.05, "detune matches the equivalent frequency");

        // Automated detune reaches it too.
        f.frequency.value = 500;
        out = ctx.renderBlock(4096);
        near(peak(out, 2048, 4096), low, low * 0.1 + 1e-6, "closed again");
        f.detune.setValueAtTime(2400, ctx.currentTime + 2048 / sr);
        out = ctx.renderBlock(4096);
        expect(peak(out, 256, 1792) < open * 0.5, "closed before the scheduled detune");
        out = ctx.renderBlock(4096);
        near(peak(out, 2048, 4096), open, open * 0.05, "open after the scheduled detune");

        const mag = new Float32Array(1), phase = new Float32Array(1);
        f.getFrequencyResponse(new Float32Array([2000]), mag, phase);
        near(mag[0], 1, 0.1, "response uses the automated detune");

        osc.stop();
        settle();
        f.disconnect();
        return "SUCCESS";
    )JS"));
}

static void test_buffer_source() {
    runScript("AudioBufferSourceNode rate is live and honours the buffer's rate", withPrelude(R"JS(
        // One second of 441 Hz: a whole number of cycles, so it loops cleanly.
        const src = ctx.createBufferSource();
        src.buffer = sineBuffer(sr, sr, 441, 0, sr);
        src.loop = true;
        src.connect(ctx.destination);
        src.start();
        let out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 512, 4096), 441, "buffer at rate 1");
        src.playbackRate.value = 2;
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 1024, 4096), 882, "playbackRate set after start");
        src.detune.value = -1200;
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 1024, 4096), 441, "detune set after start");
        src.stop();
        ctx.renderBlock(1024);

        // A buffer at half the context rate plays at its own pitch.
        const half = ctx.createBufferSource();
        half.buffer = sineBuffer(sr / 2, sr / 2, 441, 0, sr / 2);
        half.loop = true;
        half.connect(ctx.destination);
        half.start();
        out = ctx.renderBlock(4096);
        nearPitch(pitch(out, 512, 4096), 441, "half-rate buffer at its own pitch");
        half.stop();
        ctx.renderBlock(1024);
        return "SUCCESS";
    )JS"));

    runScript("AudioBufferSourceNode loop window", withPrelude(R"JS(
        // Silence, then 2400 frames of a 240-frame-period sine (whole
        // cycles), then silence. Looping the sine region is continuous
        // sound; looping the whole buffer has silent stretches.
        const L = 4800, ls = 1200, le = 3600;
        function windowPeaks(out) {
            const w = [];
            for (let i = 0; i + 128 <= out.length; i += 128) w.push(peak(out, i, i + 128));
            return w;
        }
        const buf = sineBuffer(L, sr, sr / 240, ls, le);
        const ref = refLevel();

        const whole = ctx.createBufferSource();
        whole.buffer = buf;
        whole.loop = true;
        whole.connect(ctx.destination);
        whole.start();
        ctx.renderBlock(4096);
        let w = windowPeaks(ctx.renderBlock(4096));
        expect(Math.min(...w) < ref * 0.05, "control: whole-buffer loop has silent stretches");
        whole.stop();
        ctx.renderBlock(1024);

        const src = ctx.createBufferSource();
        src.buffer = buf;
        src.loop = true;
        src.loopStart = ls / sr;
        src.loopEnd = le / sr;
        src.connect(ctx.destination);
        src.start(0, ls / sr);
        for (let k = 0; k < 3; k++) {
            w = windowPeaks(ctx.renderBlock(4096));
            if (k === 0) w = w.slice(2);  // the playback's start fade-in
            expect(Math.min(...w) > ref * 0.3, "loop window plays continuously (block " + k + "): " + w.map(x => (x / ref).toFixed(2)).join(" "));
        }

        // Moving the window while playing: loopEnd = 0 means the whole buffer.
        src.loopEnd = 0;
        ctx.renderBlock(4096);
        w = windowPeaks(ctx.renderBlock(4096));
        expect(Math.min(...w) < ref * 0.05, "live loopEnd = 0 loops the whole buffer again");
        src.stop();
        ctx.renderBlock(1024);
        return "SUCCESS";
    )JS"));

    runScript("AudioBufferSourceNode start offset and duration", withPrelude(R"JS(
        // Half a second: silence, then a sine from 0.25 s.
        const quarter = sr / 4;
        const buf = sineBuffer(sr / 2, sr, 441, quarter, sr / 2);
        const ref = refLevel(), loud = ref * 0.5, silent = ref * 0.05;

        const plain = ctx.createBufferSource();
        plain.buffer = buf;
        plain.connect(ctx.destination);
        plain.start();
        expect(peak(ctx.renderBlock(1024), 0, 1024) < silent, "control: no offset starts in silence");
        plain.stop();
        ctx.renderBlock(1024);

        const src = ctx.createBufferSource();
        src.buffer = buf;
        src.connect(ctx.destination);
        const events = [];
        src.onended = function (e) {
            churn();
            events.push([this === src, e.type, e.target === src, e.currentTarget === src]);
        };
        // From 0.25 s in, for 0.05 s of content.
        src.start(0, 0.25, 0.05);
        const durFrames = Math.round(0.05 * sr);
        let out = ctx.renderBlock(1024);
        expect(peak(out, 128, 1024) > loud, "offset starts in the sine");
        let rendered = 1024;
        while (rendered + 1024 < durFrames - 256) {
            out = ctx.renderBlock(1024);
            rendered += 1024;
            expect(peak(out, 0, 1024) > loud, "sounding within the duration at " + rendered);
            expect(events.length === 0, "no onended before the duration");
        }
        ctx.renderBlock(2048);
        out = ctx.renderBlock(2048);
        expect(peak(out, 0, 2048) < silent, "silent after the duration");
        expect(events.length === 1, "onended fired once: " + events.length);
        const ev0 = events[0];
        expect(ev0[0] && ev0[1] === "ended" && ev0[2] && ev0[3], "onended this/event: " + ev0.join(","));

        // Duration counts content across loops: a 0.05 s loop for 0.2 s.
        const loopEnded = [];
        const looped = ctx.createBufferSource();
        looped.buffer = sineBuffer(sr / 20, sr, 440, 0, sr / 20);
        looped.loop = true;
        looped.connect(ctx.destination);
        looped.onended = () => loopEnded.push(ctx.currentTime);
        looped.start(0, 0, 0.2);
        ctx.renderBlock(4096);
        out = ctx.renderBlock(4096);           // 0.085 .. 0.17 s at 48 kHz: past the buffer
        expect(peak(out, 0, 4096) > loud, "still looping past one buffer length");
        expect(loopEnded.length === 0, "looping source not ended yet");
        for (let k = 0; k < 4; k++) ctx.renderBlock(4096);
        expect(loopEnded.length === 1, "looping source ended after its duration");

        // stop() ends the source: onended on the next render.
        const stopped = [];
        const s2 = ctx.createBufferSource();
        s2.buffer = buf;
        s2.loop = true;
        s2.connect(ctx.destination);
        s2.onended = () => stopped.push(1);
        s2.start();
        ctx.renderBlock(256);
        s2.stop();
        ctx.renderBlock(128);
        expect(stopped.length === 1, "stop() fires onended");

        // Negative arguments are RangeErrors and do not start the node.
        const bad = ctx.createBufferSource();
        bad.buffer = buf;
        for (const args of [[-1], [0, -1], [0, 0, -1]]) {
            let threw = null;
            try { bad.start(...args); } catch (e) { threw = e; }
            expect(threw instanceof RangeError, "start(" + args.join(",") + ") RangeError: " + threw);
        }
        bad.start();
        bad.stop();
        ctx.renderBlock(128);
        return "SUCCESS";
    )JS"));
}

static void collectNow() {
    ev::drainMicrotasks();
    ev::collectGarbage();
    ev::drainFinalizers();
    ev::collectGarbage();
}

static void test_playing_source_is_held() {
    // Web Audio keeps a playing source alive with no script reference; the
    // binding holds it until it ends, fires onended, then lets it go.
    runScript("an unreferenced playing source: start", withPrelude(R"JS(
        (function () {
            const src = ctx.createBufferSource();
            src.buffer = sineBuffer(sr / 10, sr, 441, 0, sr / 10);
            src.connect(ctx.destination);
            src.onended = () => { globalThis.__endedCount = (globalThis.__endedCount || 0) + 1; };
            src.start();
            globalThis.__playing = new WeakRef(src);
        })();
        ctx.renderBlock(128);
        return "SUCCESS";
    )JS"));
    collectNow();
    runScript("an unreferenced playing source: plays to its end", withPrelude(R"JS(
        expect(globalThis.__playing.deref() !== undefined, "playing source still alive");
        expect(peak(ctx.renderBlock(1024), 0, 1024) > 1e-4, "still sounding");
        for (let k = 0; k < 3; k++) ctx.renderBlock(4096);
        expect(globalThis.__endedCount === 1, "onended ran: " + globalThis.__endedCount);
        return "SUCCESS";
    )JS"));
    collectNow();
    runScript("an unreferenced playing source: released after it ended", withPrelude(R"JS(
        expect(globalThis.__playing.deref() === undefined, "ended source was collected");
        return "SUCCESS";
    )JS"));
}

static void test_is_clip_playing() {
    runScript("isClipPlaying answers from the playback's state", withPrelude(R"JS(
        const clip = ctx.createClip(sineBuffer(sr / 10, sr, 441, 0, sr / 10));   // 0.1 s
        expect(clip >= 0, "clip");

        const id = ctx.playClip(clip, 0.5, false);
        expect(ctx.isClipPlaying(id), "playing at position 0, before any render");
        ctx.setPlaybackPlaying(id, false);
        expect(!ctx.isClipPlaying(id), "paused is not playing");
        ctx.setPlaybackPlaying(id, true);
        ctx.renderBlock(1024);
        expect(ctx.isClipPlaying(id), "playing mid-clip");
        ctx.stopClip(id);
        expect(!ctx.isClipPlaying(id), "stopped");

        const sched = ctx.playClip(clip, 0.5, false, ctx.currentTime + 0.05);
        expect(!ctx.isClipPlaying(sched), "scheduled in the future is not playing yet");
        for (let k = 0; k < 2; k++) ctx.renderBlock(2048);            // past +0.05 s
        expect(ctx.isClipPlaying(sched), "playing once the clock reaches when");
        for (let k = 0; k < 3; k++) ctx.renderBlock(4096);
        expect(!ctx.isClipPlaying(sched), "finished");

        const looped = ctx.playClip(clip, 0.5, true);
        for (let k = 0; k < 3; k++) ctx.renderBlock(4096);
        expect(ctx.isClipPlaying(looped), "a looping clip keeps playing");
        ctx.stopClip(looped);
        expect(!ctx.isClipPlaying(-1) && !ctx.isClipPlaying(123456), "unknown ids");
        ctx.renderBlock(1024);
        return "SUCCESS";
    )JS"));
}

int main() {
    std::cout << "Running broaudio API live-param tests..." << std::endl;
    const char* stress = std::getenv("BRONZE_GC_STRESS");
    std::cout << "  (BRONZE_GC_STRESS=" << (stress ? stress : "unset") << ")" << std::endl;

    broaudio::Engine engine;
    TEST_CHECK(engine.initHeadless());
    broaudio::api::setAudioEngine(&engine);

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        test_oscillator_live();
        test_biquad_detune();
        test_buffer_source();
        test_playing_source_is_held();
        test_is_clip_playing();
        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio API live-param tests passed!" << std::endl;
    return 0;
}
