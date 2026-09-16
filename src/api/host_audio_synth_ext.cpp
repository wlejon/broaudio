// The engine-level synth methods of AudioContext that host_audio_context.cpp
// and host_audio_synth.cpp leave out: the shared ModMatrix, wavetable banks,
// the output spectrum, deterministic block rendering, and the four preset
// families (voice / bus / mod / engine) with their JSON and file forms.
// Paths go to the engine as given; the caller resolves them.

#include "host_audio_internal.h"

#include <vector>

namespace broaudio::api {

void registerAudioContextSynthExt(ObjectBuilder& b) {
    b.def("getModMatrix", 0, [](Value, std::span<const Value>) {
        return makeModMatrixValue();
    });

    // ---- Wavetables --------------------------------------------------------
    b.def("createWavetable", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::fromDouble(-1);
        const uint8_t* rawData = nullptr;
        size_t rawLen = 0, elemSize = 1;
        if (!bufferBytes(a[0], &rawData, &rawLen, &elemSize) || rawLen == 0) return ev::fromDouble(-1);
        int sr = a.size() >= 2 ? i32At(a, 1) : 44100;
        int count = static_cast<int>(rawLen / sizeof(float));
        auto bank = broaudio::WavetableBank::createFromWaveform(reinterpret_cast<const float*>(rawData), count, sr);
        return ev::fromDouble(registerWavetable(bank));
    });

    b.def("createWavetableFromWaveform", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::fromDouble(-1);
        const uint8_t* rawData = nullptr;
        size_t rawLen = 0, elemSize = 1;
        if (!bufferBytes(a[0], &rawData, &rawLen, &elemSize) || rawLen == 0) return ev::fromDouble(-1);
        int count = a.size() >= 2 ? i32At(a, 1) : static_cast<int>(rawLen / sizeof(float));
        int sr = a.size() >= 3 ? i32At(a, 2) : 44100;
        auto bank = broaudio::WavetableBank::createFromWaveform(reinterpret_cast<const float*>(rawData), count, sr);
        return ev::fromDouble(registerWavetable(bank));
    });

    b.def("deleteWavetable", 1, [](Value, std::span<const Value> a) {
        if (!a.empty()) deleteWavetable(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setVoiceWavetable", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceWavetable(i32At(a, 0), findWavetable(i32At(a, 1)));
        return ev::undefined();
    });

    // ---- Analysis and rendering --------------------------------------------
    b.def("getSpectrum", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::null();
        int bins = i32At(a, 0);
        if (bins <= 0) return ev::null();
        std::vector<float> spec = e->getSpectrum(bins);
        if (spec.empty()) return ev::null();
        return makeFloat32Array(spec);
    });

    b.def("renderBlock", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->renderBlock(i32At(a, 0));
        return ev::undefined();
    });

    // ---- Presets -----------------------------------------------------------
    b.def("voicePresetToJson", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("{}");
        return ev::fromUtf8(broaudio::toJson(broaudio::voicePresetFromJson(ev::toUtf8(a[0]))));
    });

    b.def("busPresetToJson", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("{}");
        return ev::fromUtf8(broaudio::toJson(broaudio::busPresetFromJson(ev::toUtf8(a[0]))));
    });

    b.def("modPresetToJson", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("{}");
        return ev::fromUtf8(broaudio::toJson(broaudio::modPresetFromJson(ev::toUtf8(a[0]))));
    });

    b.def("enginePresetToJson", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("{}");
        return ev::fromUtf8(broaudio::toJson(broaudio::enginePresetFromJson(ev::toUtf8(a[0]))));
    });

    b.def("applyVoicePreset", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->applyVoicePreset(i32At(a, 0), broaudio::voicePresetFromJson(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("applyBusPreset", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->applyBusPreset(i32At(a, 0), broaudio::busPresetFromJson(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("applyModPreset", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->applyModPreset(broaudio::modPresetFromJson(ev::toUtf8(a[0])));
        return ev::undefined();
    });

    b.def("applyEnginePreset", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->applyEnginePreset(broaudio::enginePresetFromJson(ev::toUtf8(a[0])));
        return ev::undefined();
    });

    b.def("savePreset", 2, [](Value, std::span<const Value> a) {
        if (a.size() < 2) return ev::fromBool(false);
        std::string json = ev::toUtf8(a[0]);
        std::string path = ev::toUtf8(a[1]);
        return ev::fromBool(broaudio::savePresetToFile(json, path.c_str()));
    });

    b.def("loadPreset", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("");
        std::string path = ev::toUtf8(a[0]);
        return ev::fromUtf8(broaudio::loadPresetFromFile(path.c_str()));
    });
}

} // namespace broaudio::api
