// Behavioural checks for broaudio_api against the pre-bronze (QuickJS)
// binding: every row of bro's docs/transition-drift.md section B, plus the
// node-level restorations the static shape diff found (analyser `source`,
// oscillator gain-through-connect, biquad connect/disconnect, the filter
// slot error, the sequence note `beat` key).
//
// Runs on its own headless broaudio::Engine so renderBlock() drives the
// pipeline deterministically, and a path resolver that maps "vfs/..." into
// a scratch directory so the file rows are checked end to end.

#include "api.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "broaudio/engine.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace ev = bronze::embed;
using Value = bronze::Value;

#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::exit(1); \
    } \
} while (0)

static std::filesystem::path g_scratch;

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

// Shared JS prelude: one AudioContext per script plus small helpers.
static const char* kPrelude = R"JS(
    const ctx = new AudioContext();
    const sr = ctx.sampleRate;
    function sine(frames, hz, rate) {
        const out = new Float32Array(frames);
        for (let i = 0; i < frames; i++) out[i] = 0.5 * Math.sin(2 * Math.PI * hz * i / (rate || sr));
        return out;
    }
    function maxAbs(arr) { let m = 0; for (let i = 0; i < arr.length; i++) m = Math.max(m, Math.abs(arr[i])); return m; }
    function expectTypeError(fn, what) {
        let ok = false;
        try { fn(); } catch (e) { ok = e instanceof TypeError; if (!ok) throw new Error(what + " threw " + e); }
        if (!ok) throw new Error(what + " should throw TypeError");
    }
    function flush() { for (let i = 0; i < 8; i++) ctx.renderBlock(4096); }
)JS";

static std::string withPrelude(const char* body) {
    return std::string("(function() {") + kPrelude + body + "\n})()";
}

static void test_clips() {
    // B1 playClip(clip, gain, loop, when), B2 createClip srcRate resampling,
    // B7 getClipWaveform always a Float32Array.
    runScript("B1/B2/B7 clips", withPrelude(R"JS(
        const clipNative = ctx.createClip(sine(22050, 440), 1);
        if (ctx.getClipSampleCount(clipNative) !== 22050) throw new Error("createClip without srcRate resampled: " + ctx.getClipSampleCount(clipNative));
        const clipHalf = ctx.createClip(sine(22050, 440, 22050), 1, 22050);
        const n = ctx.getClipSampleCount(clipHalf);
        if (!(n > 22050 * 1.8 && n < 22050 * 2.2)) throw new Error("createClip(samples, ch, 22050) at " + sr + " gave " + n + " samples");

        const clip = ctx.createClip(sine(sr, 220), 1);
        const now = ctx.playClip(clip, 1.0, false);
        const later = ctx.playClip(clip, 1.0, false, ctx.currentTime + 5);
        if (typeof now !== "number" || typeof later !== "number") throw new Error("playClip ids");
        ctx.renderBlock(4096);
        const posNow = ctx.getPlaybackPositionSeconds(now);
        const posLater = ctx.getPlaybackPositionSeconds(later);
        if (!(posNow > 0)) throw new Error("immediate playback did not advance: " + posNow);
        if (posLater > 0) throw new Error("playback scheduled 5 s ahead advanced: " + posLater);
        ctx.stopPlayback(now); ctx.stopPlayback(later);

        const wf = ctx.getClipWaveform(clip, 64);
        if (!(wf instanceof Float32Array) || wf.length !== 128) throw new Error("getClipWaveform shape: " + wf);
        if (ctx.getClipWaveform(clip) !== undefined) throw new Error("getClipWaveform(clip) should be undefined");
        if (ctx.getClipWaveform(clip, 0) !== undefined) throw new Error("getClipWaveform(clip, 0) should be undefined");
        if (ctx.getClipWaveform(clip, 2048) !== undefined) throw new Error("getClipWaveform(clip, 2048) should be undefined");
        return "SUCCESS";
    )JS"));
}

static void test_files() {
    // B3 path resolver on saveWav / createClipFromFile / decodeAudioFile,
    // B6 createStreamFromFile options, B15 TypeErrors.
    runScript("B3/B6/B15 files", withPrelude(R"JS(
        if (!ctx.saveWav("vfs/tone.wav", sine(4410, 440), 1, sr)) throw new Error("saveWav returned false");
        expectTypeError(() => ctx.saveWav("vfs/bad.wav", [1, 2, 3], 1, sr), "saveWav(array)");

        const clip = ctx.createClipFromFile("vfs/tone.wav");
        if (!(clip >= 0)) throw new Error("createClipFromFile through resolver: " + clip);
        if (ctx.getClipSampleCount(clip) !== 4410) throw new Error("round-tripped clip length " + ctx.getClipSampleCount(clip));
        const decoded = ctx.decodeAudioFile("vfs/tone.wav");
        if (!decoded) throw new Error("decodeAudioFile through resolver returned " + decoded);

        const stream = ctx.createStreamFromFile("vfs/tone.wav", { prebufferFrames: 256, gain: 0.5, loop: false });
        if (!(stream >= 0)) throw new Error("createStreamFromFile: " + stream);
        const stats = ctx.getStreamStats(stream);
        if (!stats || typeof stats.bufferedFrames !== "number") throw new Error("getStreamStats: " + JSON.stringify(stats));
        ctx.closeStream(stream);
        expectTypeError(() => ctx.createStreamFromFile(), "createStreamFromFile()");
        let threw = false;
        try { ctx.createStreamFromFile("vfs/missing.wav"); } catch (e) { threw = true; }
        if (!threw) throw new Error("createStreamFromFile(missing) should throw");
        return "SUCCESS";
    )JS"));
    TEST_CHECK(std::filesystem::exists(g_scratch / "tone.wav"));
}

static void test_async() {
    // B4 createClipFromFileAsync: a Promise settled from the frame pump.
    std::cout << "  B4 createClipFromFileAsync..." << std::endl;
    ev::CallResult res = bronze::eval::evalScript(withPrelude(R"JS(
        globalThis.__asyncClip = -2; globalThis.__asyncErr = null;
        globalThis.__missingErr = null;
        expectTypeError(() => ctx.createClipFromFileAsync(), "createClipFromFileAsync()");
        const p = ctx.createClipFromFileAsync("vfs/tone.wav");
        if (!p || typeof p.then !== "function") throw new Error("createClipFromFileAsync did not return a Promise");
        p.then(id => { globalThis.__asyncClip = id; }, e => { globalThis.__asyncErr = String(e && e.message || e); });
        ctx.createClipFromFileAsync("vfs/nope.wav").then(() => { globalThis.__missingErr = "resolved"; },
                                                          e => { globalThis.__missingErr = String(e && e.message || e); });
        globalThis.__ctx = ctx;
        return "SUCCESS";
    )JS"));
    TEST_CHECK(!res.thrown);

    for (int i = 0; i < 2000; i++) {
        broaudio::api::tickAsyncJobs();
        ev::drainMicrotasks();
        Value id = ev::globalValue("__asyncClip").value;
        Value missing = ev::globalValue("__missingErr").value;
        if (ev::isNumber(id) && ev::toDouble(id) != -2 && ev::isString(missing)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Value err = ev::globalValue("__asyncErr").value;
    if (ev::isString(err)) {
        std::cerr << "async load rejected: " << ev::toUtf8(err) << std::endl;
        std::exit(1);
    }
    Value id = ev::globalValue("__asyncClip").value;
    TEST_CHECK(ev::isNumber(id) && ev::toDouble(id) >= 0);
    Value missing = ev::globalValue("__missingErr").value;
    TEST_CHECK(ev::isString(missing));
    std::string missingMsg = ev::toUtf8(missing);
    TEST_CHECK(missingMsg != "resolved");
    // The rejection names the resolved path, as the old binding did.
    TEST_CHECK(missingMsg.find("nope.wav") != std::string::npos);
}

static void test_streams() {
    // B5 createStream ringFrames default 0 (= two seconds), B15 TypeError.
    runScript("B5/B15 streams", withPrelude(R"JS(
        const s = ctx.createStream(1);
        if (!(s >= 0)) throw new Error("createStream: " + s);
        const frames = Math.floor(sr * 1.5);
        const wrote = ctx.pushStreamSamples(s, new Float32Array(frames));
        if (wrote !== frames) throw new Error("default ring took " + wrote + " of " + frames + " frames");
        expectTypeError(() => ctx.pushStreamSamples(s, [0, 0, 0]), "pushStreamSamples(array)");
        ctx.closeStream(s);
        return "SUCCESS";
    )JS"));
}

static void test_render_and_wavetables() {
    // B8 renderBlock returns the mixdown, B9/B10 wavetable constructors,
    // B11 setVoiceWavetable switches the waveform, B17 getSpectrum bound.
    runScript("B8-B11/B17 render + wavetables", withPrelude(R"JS(
        if (ctx.renderBlock() !== undefined || ctx.renderBlock(0) !== undefined) throw new Error("renderBlock bad args");

        function renderVoice(setup) {
            const v = ctx.createVoice();
            ctx.setVoiceFrequency(v, 220);
            ctx.setVoiceGain(v, 0.5);
            ctx.setVoiceAttackTime(v, 0);
            setup(v);
            ctx.startVoice(v);
            ctx.renderBlock(2048);   // settle the master chain's dynamics
            const out = new Float32Array(ctx.renderBlock(2048));
            ctx.removeVoice(v);
            flush();
            return out;
        }
        const sineOut = renderVoice(() => {});
        if (sineOut.length !== 2048) throw new Error("renderBlock length " + sineOut.length);
        if (!(maxAbs(sineOut) > 0.005)) throw new Error("renderBlock returned silence (peak " + maxAbs(sineOut) + ")");

        const buf = new Float32Array(256);
        if (ctx.renderBlock(256, buf) !== buf) throw new Error("renderBlock(n, out) should return out");

        const saw = ctx.createWavetable("saw");
        const square = ctx.createWavetable("square");
        const tri = ctx.createWavetable("triangle");
        if (typeof saw !== "number" || typeof square !== "number" || typeof tri !== "number") throw new Error("createWavetable(type)");
        if (ctx.createWavetable("nope") !== undefined) throw new Error("createWavetable(unknown) should be undefined");
        const cycle = new Float32Array(256);
        for (let i = 0; i < 256; i++) cycle[i] = (i < 128) ? 1 : -1;
        const custom = ctx.createWavetableFromWaveform(cycle);
        if (typeof custom !== "number") throw new Error("createWavetableFromWaveform");
        expectTypeError(() => ctx.createWavetableFromWaveform([1, -1]), "createWavetableFromWaveform(array)");

        const wtOut = renderVoice(v => ctx.setVoiceWavetable(v, saw));
        let diff = 0;
        for (let i = 0; i < 2048; i++) diff = Math.max(diff, Math.abs(wtOut[i] - sineOut[i]));
        if (!(diff > 0.005)) throw new Error("setVoiceWavetable left the voice on sine (max diff " + diff + ")");
        ctx.deleteWavetable(saw); ctx.deleteWavetable(square); ctx.deleteWavetable(tri); ctx.deleteWavetable(custom);

        const spec = ctx.getSpectrum(8192);
        if (!(spec instanceof Float32Array) || spec.length !== 8192) throw new Error("getSpectrum(8192)");
        if (ctx.getSpectrum(8193) !== undefined) throw new Error("getSpectrum(8193) should be undefined");
        if (ctx.getSpectrum(0) !== undefined) throw new Error("getSpectrum(0) should be undefined");
        return "SUCCESS";
    )JS"));
}

static void test_presets() {
    // B12 toJson takes objects, B13 fromJson returns objects, save/load
    // through the path resolver.
    runScript("B12/B13 presets", withPrelude(R"JS(
        const json = ctx.voicePresetToJson({ waveform: "square", frequency: 110, gain: 0.25, unisonCount: 3 });
        if (typeof json !== "string") throw new Error("voicePresetToJson returned " + typeof json);
        const back = ctx.voicePresetFromJson(json);
        if (!back || back.waveform !== "square" || back.frequency !== 110 || back.gain !== 0.25 || back.unisonCount !== 3)
            throw new Error("voice preset round trip: " + JSON.stringify(back));
        if (ctx.voicePresetToJson() !== null) throw new Error("voicePresetToJson() should be null");

        const eng = ctx.enginePresetFromJson(ctx.enginePresetToJson({ masterGain: 0.75 }));
        if (!eng || eng.masterGain !== 0.75) throw new Error("engine preset round trip: " + JSON.stringify(eng));
        const bus = ctx.busPresetFromJson(ctx.busPresetToJson({ gain: 0.5, effectOrder: ["reverb", "delay"] }));
        if (!bus || bus.gain !== 0.5 || !Array.isArray(bus.effectOrder) || bus.effectOrder[0] !== "reverb")
            throw new Error("bus preset round trip: " + JSON.stringify(bus));
        const mod = ctx.modPresetFromJson(ctx.modPresetToJson({ lfos: [{ rate: 3 }] }));
        if (!mod || !Array.isArray(mod.lfos)) throw new Error("mod preset round trip: " + JSON.stringify(mod));

        const v = ctx.createVoice();
        ctx.applyVoicePreset(v, back);
        ctx.applyEnginePreset({ masterGain: 1 });
        ctx.removeVoice(v);

        if (!ctx.savePreset(json, "vfs/voice.json")) throw new Error("savePreset returned false");
        if (ctx.loadPreset("vfs/voice.json") !== json) throw new Error("loadPreset did not round trip");
        if (ctx.loadPreset("vfs/missing.json") !== null) throw new Error("loadPreset(missing) should be null");
        return "SUCCESS";
    )JS"));
    TEST_CHECK(std::filesystem::exists(g_scratch / "voice.json"));
}

static void test_master_setters() {
    // B14 master chorus/compressor setters, setBusChorusBaseDelay,
    // setPlaybackSend; B16 setBusEffectOrder; B15 TypeErrors.
    runScript("B14/B15/B16 setters", withPrelude(R"JS(
        const names = ["setChorusEnabled", "setChorusRate", "setChorusDepth", "setChorusMix", "setChorusFeedback",
                       "setChorusBaseDelay", "setCompressorEnabled", "setCompressorThreshold", "setCompressorRatio",
                       "setCompressorAttack", "setCompressorRelease", "setBusChorusBaseDelay", "getBusChorusBaseDelay",
                       "setPlaybackSend", "setBusEffectOrder"];
        for (const n of names) if (typeof ctx[n] !== "function") throw new Error("missing ctx." + n);
        ctx.setChorusEnabled(true); ctx.setChorusRate(1.5); ctx.setChorusDepth(0.3); ctx.setChorusMix(0.5);
        ctx.setChorusFeedback(0.2); ctx.setChorusBaseDelay(0.01);
        ctx.setCompressorEnabled(true); ctx.setCompressorThreshold(-12); ctx.setCompressorRatio(4);
        ctx.setCompressorAttack(0.01); ctx.setCompressorRelease(0.1);
        const bus = ctx.createBus();
        ctx.setBusChorusBaseDelay(bus, 0.02);
        if (Math.abs(ctx.getBusChorusBaseDelay(bus) - 0.02) > 1e-4) throw new Error("bus chorus base delay " + ctx.getBusChorusBaseDelay(bus));
        ctx.setBusEffectOrder(bus, ["reverb", "delay", "chorus"]);
        ctx.setBusEffectOrder(bus, ["a", "b", "c", "d", "e", "f", "g", "h"]);
        ctx.setBusEffectOrder(bus, []);
        ctx.deleteBus(bus);

        const clip = ctx.createClip(sine(2048, 440), 1);
        const pb = ctx.playClip(clip, 1.0, false);
        ctx.setPlaybackSend(pb, 0, 0.5);
        ctx.stopPlayback(pb);

        expectTypeError(() => ctx.createMediaStreamSource({}), "createMediaStreamSource({})");
        if (ctx.createMediaStreamSource() !== undefined) throw new Error("createMediaStreamSource() should be undefined");
        expectTypeError(() => ctx.createSequence({}), "createSequence({})");
        if (ctx.createSequence() !== undefined) throw new Error("createSequence() should be undefined");
        const seq = ctx.createSequence(ctx.createVoiceAllocator());
        seq.addNote(1.5, 60, 0.8, 0.25);
        const note = seq.note(0);
        if (!note || note.beat !== 1.5 || note.beatPosition !== 1.5) throw new Error("seq.note(0): " + JSON.stringify(note));
        return "SUCCESS";
    )JS"));
}

static void test_nodes() {
    // Analyser `source`, oscillator gain through connect(), biquad
    // connect/disconnect, filter slot exhaustion.
    runScript("nodes: analyser source / osc gain / biquad", withPrelude(R"JS(
        const analyser = ctx.createAnalyser();
        if (analyser.source !== 0) throw new Error("analyser.source default " + analyser.source);
        analyser.source = 2; if (analyser.source !== 2) throw new Error("analyser.source = 2");
        analyser.source = 7; if (analyser.source !== 0) throw new Error("analyser.source = 7 should clamp to 0");
        if (typeof MediaStreamAudioSourceNode.prototype.connect !== "function") throw new Error("MediaStreamAudioSourceNode.connect");

        function oscPeak(gainValue) {
            const osc = ctx.createOscillator();
            const gain = ctx.createGain();
            osc.connect(gain).connect(ctx.destination);
            gain.gain.value = gainValue;
            osc.start();
            ctx.renderBlock(2048);   // the master chain's first block settles its dynamics
            ctx.renderBlock(2048);
            const td = new Float32Array(analyser.fftSize);
            analyser.source = 0;
            analyser.getFloatTimeDomainData(td);
            const peakOut = maxAbs(td);
            analyser.source = 1;
            analyser.getFloatTimeDomainData(td);
            const peakMic = maxAbs(td);
            osc.stop();
            flush();
            const tail = maxAbs(new Float32Array(ctx.renderBlock(2048)));
            return { out: peakOut, mic: peakMic, tail: tail };
        }
        const loud = oscPeak(1.0);
        const quiet = oscPeak(0.25);
        if (!(loud.out > 0.01)) throw new Error("oscillator silent: " + loud.out);
        if (!(quiet.out < loud.out * 0.5)) throw new Error("gain.gain.value ignored: " + quiet.out + " vs " + loud.out);
        if (loud.tail !== 0 || quiet.tail !== 0) throw new Error("oscillator kept sounding after stop()");
        if (loud.mic !== 0) throw new Error("analyser.source = 1 read the output ring: " + loud.mic);

        const filter = ctx.createBiquadFilter();
        filter.connect(ctx.destination);
        filter.disconnect();
        const filters = [];
        let slotError = null;
        try { for (let i = 0; i < 64; i++) filters.push(ctx.createBiquadFilter()); } catch (e) { slotError = e; }
        if (!slotError || slotError instanceof TypeError || slotError.message !== "No filter slots available")
            throw new Error("createBiquadFilter past the slot cap: " + slotError);
        // Take the master lowpasses back out of the chain for the later tests.
        for (const f of filters) f.disconnect();
        const probe = ctx.createOscillator();
        probe.connect(ctx.destination);
        probe.start();
        ctx.renderBlock(2048);
        const afterFilters = maxAbs(new Float32Array(ctx.renderBlock(2048)));
        probe.stop();
        flush();
        if (!(afterFilters > 0.01)) throw new Error("output after disconnecting filters: " + afterFilters);
        return "SUCCESS";
    )JS"));
}

int main() {
    std::cout << "Running broaudio API drift tests..." << std::endl;

    g_scratch = std::filesystem::temp_directory_path() / "broaudio_api_drift";
    std::filesystem::remove_all(g_scratch);
    std::filesystem::create_directories(g_scratch);

    broaudio::Engine engine;
    TEST_CHECK(engine.initHeadless());
    broaudio::api::setAudioEngine(&engine);
    // "vfs" and "vfs/<file>" both land in the scratch dir: write paths are
    // resolved by their parent directory, read paths by the file.
    broaudio::api::setPathResolver([](const std::string& p) -> std::string {
        if (p == "vfs") return g_scratch.string();
        if (p.rfind("vfs/", 0) == 0) return (g_scratch / p.substr(4)).string();
        return p;
    });

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        test_clips();
        test_files();
        test_async();
        test_streams();
        // Level-sensitive checks run before the tests that reconfigure the
        // master chain (compressor, chorus, engine presets).
        test_nodes();
        test_render_and_wavetables();
        test_presets();
        test_master_setters();
        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();

    std::cout << "All broaudio API drift tests passed!" << std::endl;
    return 0;
}
