// The engine-level synth methods of AudioContext that host_audio_context.cpp
// and host_audio_synth.cpp leave out: the shared ModMatrix, wavetable banks,
// the output spectrum and deterministic block rendering. The preset families
// live in host_audio_presets.cpp.

#include "host_audio_internal.h"

#include <vector>

namespace broaudio::api {

namespace {

// A Float32Array of `count` zeros with the first min(count, src.size())
// values of `src` copied in — the shape every analysis read hands back, so
// a caller can index it without a null check.
Value zeroPaddedFloat32Array(size_t count, const std::vector<float>& src) {
    std::vector<float> out(count, 0.0f);
    size_t n = std::min(count, src.size());
    if (n > 0) std::memcpy(out.data(), src.data(), n * sizeof(float));
    return makeFloat32Array(out);
}

}  // namespace

void registerAudioContextSynthExt(ObjectBuilder& b) {
    b.def("getModMatrix", 0, [](Value, std::span<const Value>) {
        return makeModMatrixValue();
    });

    // ---- Wavetables --------------------------------------------------------
    // createWavetable("saw" | "square" | "triangle") -> id, or undefined for
    // any other type. The bank is built at the engine sample rate.
    b.def("createWavetable", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || ev::isObject(a[0])) return ev::undefined();
        auto* e = getAudioEngine();
        int sr = e ? e->sampleRate() : 44100;
        std::string type = ev::toUtf8(a[0]);
        std::shared_ptr<broaudio::WavetableBank> bank;
        if (type == "saw") bank = broaudio::WavetableBank::createSaw(sr);
        else if (type == "square") bank = broaudio::WavetableBank::createSquare(sr);
        else if (type == "triangle") bank = broaudio::WavetableBank::createTriangle(sr);
        if (!bank) return ev::undefined();
        return ev::fromDouble(registerWavetable(bank));
    });

    // createWavetableFromWaveform(Float32Array oneCycle) -> id. The bank is
    // built at the engine sample rate; the whole view is the cycle.
    b.def("createWavetableFromWaveform", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
        if (!info || !info.data) return ev::throwTypeError("Expected Float32Array");
        auto* e = getAudioEngine();
        int sr = e ? e->sampleRate() : 44100;
        int count = static_cast<int>(info.byteLength / sizeof(float));
        auto bank = broaudio::WavetableBank::createFromWaveform(
            reinterpret_cast<const float*>(info.data), count, sr);
        if (!bank) return ev::undefined();
        return ev::fromDouble(registerWavetable(bank));
    });

    b.def("deleteWavetable", 1, [](Value, std::span<const Value> a) {
        if (!a.empty()) deleteWavetable(i32At(a, 0));
        return ev::undefined();
    });

    // Assign the bank AND switch the voice to wavetable mode: a bank on a
    // voice still set to "sine" is inaudible, so the two are one call.
    b.def("setVoiceWavetable", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) {
            int voiceId = i32At(a, 0);
            if (auto bank = findWavetable(i32At(a, 1))) {
                e->setVoiceWavetable(voiceId, bank);
                e->setWaveform(voiceId, broaudio::Waveform::Wavetable);
            }
        }
        return ev::undefined();
    });

    // ---- Analysis and rendering --------------------------------------------
    // getSpectrum(numBins) -> Float32Array(numBins), zero-filled beyond what
    // the engine answers; undefined for numBins outside 1..8192.
    b.def("getSpectrum", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::undefined();
        int bins = i32At(a, 0);
        if (bins <= 0 || bins > 8192) return ev::undefined();
        return zeroPaddedFloat32Array(static_cast<size_t>(bins), e->getSpectrum(bins));
    });

    // renderBlock(numFrames, out?) -> Float32Array. Renders numFrames through
    // the full pipeline (no device) and returns the latest mono mixdown: a
    // fresh Float32Array(min(numFrames, analysis ring)) or, when `out` is a
    // typed array, `out` itself filled in place up to its length. Headless
    // only — driving the pipeline from the main thread while a live device
    // callback runs would race.
    b.def("renderBlock", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::undefined();
        int numFrames = i32At(a, 0);
        if (numFrames <= 0) return ev::undefined();

        e->renderBlock(numFrames);

        int cap = e->outputBuffer().capacity();
        int n = std::min(numFrames, cap);

        if (a.size() >= 2 && ev::isObject(a[1])) {
            ev::TypedArrayInfo info = ev::typedArrayInfo(a[1]);
            if (info && info.data) {
                int rc = std::min(n, static_cast<int>(info.byteLength / sizeof(float)));
                if (rc > 0) e->outputBuffer().readLatest(reinterpret_cast<float*>(info.data), rc);
                return a[1];
            }
        }

        std::vector<float> out(static_cast<size_t>(n), 0.0f);
        e->outputBuffer().readLatest(out.data(), n);
        return makeFloat32Array(out);
    });
}

} // namespace broaudio::api
