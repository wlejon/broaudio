// The AudioContext's file loaders and savers and bro.ear's clip paths under the
// host's access gate (embed/embed_access.h; src/api/api.h, "Access"): a loader
// asks Read and a saver Write about the path it was handed, a refusal is an
// Error carrying the gate's sentence, code EACCES and the absolute path (thrown,
// or createClipFromFileAsync's promise rejected), nothing is read or written,
// and with the gate removed everything behaves as before. Its own headless
// engine, like test_audio_api_drift.

#include "api.h"
#include "embed/embed.h"
#include "embed/embed_access.h"
#include "eval/eval.h"

#include <broaudio/io/audio_file.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace ev = bronze::embed;

#define GCHECK(cond, msg)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK failed: " #cond " (" << __FILE__ << ":"        \
                      << __LINE__ << "): " << (msg) << std::endl;              \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

namespace {

std::string utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The path as the gate is handed it.
std::string gatePath(const fs::path& p) {
    return utf8(fs::absolute(p).lexically_normal());
}

std::string jsStr(const fs::path& p) {
    std::string out = "'";
    for (char c : utf8(p)) {
        if (c == '\\' || c == '\'') out += '\\';
        out += c;
    }
    return out + "'";
}

std::string evalString(const std::string& src) {
    auto r = bronze::eval::evalScript(src);
    if (r.thrown) return "<thrown: " + ev::toUtf8(r.value) + ">";
    return ev::isString(r.value) ? ev::toUtf8(r.value) : "<not a string>";
}

const char* kOutcome = "e => 'denied|' + e.message + '|' + e.code + '|' + e.path";

// Runs `stmt` synchronously; "ok" or the refusal as one comparable string.
std::string syncJs(const std::string& stmt) {
    return evalString("(function() { try { " + stmt + "; return 'ok'; } catch (e) {\n  return (" +
                      kOutcome + ")(e); } })()");
}

// Runs `expr` (a promise) to completion; "ok" or the refusal string.
std::string awaitJs(const std::string& expr) {
    auto r = bronze::eval::evalScript("globalThis._gateOut = undefined;\n"
                                      "Promise.resolve().then(() => " + expr + ").then(() => 'ok', " +
                                      kOutcome + ").then(s => { globalThis._gateOut = s; });");
    if (r.thrown) return "<thrown: " + ev::toUtf8(r.value) + ">";
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        broaudio::api::tickAsyncJobs();
        ev::drainMicrotasks();
        std::string s = evalString("typeof globalThis._gateOut === 'string' ? globalThis._gateOut : ''");
        if (!s.empty()) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return "<timeout>";
}

std::string refusal(const fs::path& p) {
    const std::string path = gatePath(p);
    return "denied|no files access: " + path + "|EACCES|" + path;
}

}  // namespace

int main() {
    std::cout << "Running broaudio API access-gate test..." << std::endl;

    const fs::path root = fs::temp_directory_path() / "broaudio_api_gate";
    const fs::path ok = root / "ok";
    const fs::path hidden = root / "hidden";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(ok);
    fs::create_directories(hidden);
    {
        const int rate = 44100, frames = 4410;
        std::vector<float> s(frames);
        for (int i = 0; i < frames; ++i) s[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / rate);
        GCHECK(broaudio::saveWav(utf8(ok / "tone.wav").c_str(), s.data(), frames, 1, rate), "fixture");
        GCHECK(broaudio::saveWav(utf8(hidden / "tone.wav").c_str(), s.data(), frames, 1, rate), "fixture");
    }

    broaudio::Engine engine;
    GCHECK(engine.initHeadless(), "headless engine");
    broaudio::api::setAudioEngine(&engine);

    const std::string hiddenPrefix = gatePath(hidden);
    std::atomic<int> calls{0};
    ev::setAccessGate([&](const ev::AccessRequest& q) -> std::string {
        if (q.kind != ev::AccessKind::Read && q.kind != ev::AccessKind::Write) return "";
        ++calls;
        const std::string path(q.subject);
        return path.compare(0, hiddenPrefix.size(), hiddenPrefix) == 0 ? "no files access: " + path : "";
    });

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        broaudio::api::installEar();
        auto made = bronze::eval::evalScript("globalThis.ctx = new AudioContext(); 'ok'");
        GCHECK(!made.thrown, "new AudioContext");

        const fs::path secret = hidden / "tone.wav";
        const std::string tone = "new Float32Array(441).fill(0.25)";
        std::string r;

        // Readers.
        for (const char* call : {"createClipFromFile", "createStreamFromFile", "decodeAudioFile", "loadPreset"}) {
            r = syncJs(std::string("ctx.") + call + "(" + jsStr(secret) + ")");
            GCHECK(r == refusal(secret), std::string(call) + ": " + r);
        }
        r = syncJs("bro.ear.measure(" + jsStr(secret) + ")");
        GCHECK(r == refusal(secret), r);
        r = awaitJs("ctx.createClipFromFileAsync(" + jsStr(secret) + ")");
        GCHECK(r == refusal(secret), r);
        r = syncJs("if (!(ctx.createClipFromFile(" + jsStr(ok / "tone.wav") + ") >= 0)) throw new Error('clip')");
        GCHECK(r == "ok", r);
        r = syncJs("if (!ctx.decodeAudioFile(" + jsStr(ok / "tone.wav") + ")) throw new Error('decode')");
        GCHECK(r == "ok", r);
        r = awaitJs("ctx.createClipFromFileAsync(" + jsStr(ok / "tone.wav") + ")");
        GCHECK(r == "ok", r);

        // Writers: refused, nothing is written.
        r = syncJs("ctx.saveWav(" + jsStr(hidden / "out.wav") + ", " + tone + ", 1, 44100)");
        GCHECK(r == refusal(hidden / "out.wav"), r);
        GCHECK(!fs::exists(hidden / "out.wav"), "a refused saveWav wrote");
        r = syncJs("ctx.savePreset('{}', " + jsStr(hidden / "p.json") + ")");
        GCHECK(r == refusal(hidden / "p.json"), r);
        GCHECK(!fs::exists(hidden / "p.json"), "a refused savePreset wrote");
        r = syncJs("ctx.exportRecordingToWav(" + jsStr(hidden / "rec.wav") + ")");
        GCHECK(r == refusal(hidden / "rec.wav"), r);
        GCHECK(!fs::exists(hidden / "rec.wav"), "a refused exportRecordingToWav wrote");
        r = syncJs("bro.ear.spectrogram({samples: " + tone + ", sampleRate: 44100}, {path: " +
                   jsStr(hidden / "s.png") + "})");
        GCHECK(r == refusal(hidden / "s.png"), r);
        GCHECK(!fs::exists(hidden / "s.png"), "a refused spectrogram wrote");
        r = syncJs("if (!ctx.saveWav(" + jsStr(ok / "out.wav") + ", " + tone + ", 1, 44100)) throw new Error('save')");
        GCHECK(r == "ok", r);
        GCHECK(fs::exists(ok / "out.wav"), "an allowed saveWav did not write");

        // With the gate removed, nothing asks and everything works as before.
        ev::setAccessGate(nullptr);
        const int before = calls.load();
        r = syncJs("if (!ctx.decodeAudioFile(" + jsStr(secret) + ")) throw new Error('decode')");
        GCHECK(r == "ok", r);
        r = syncJs("if (!ctx.saveWav(" + jsStr(hidden / "out.wav") + ", " + tone + ", 1, 44100)) throw new Error('save')");
        GCHECK(r == "ok", r);
        GCHECK(fs::exists(hidden / "out.wav"), "an ungated saveWav did not write");
        GCHECK(calls.load() == before, "a removed gate was still asked");

        broaudio::api::shutdownAudio();
    }
    ev::destroyRealm(realm);
    engine.shutdown();
    fs::remove_all(root, ec);

    std::cout << "broaudio API access-gate test passed" << std::endl;
    return 0;
}
