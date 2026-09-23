// The bus effects host_audio_dsp.cpp's registerAudioContextBuses leaves
// out: chorus feedback, the graphic EQ, distortion, the per-bus effect
// order and offline processing through a bus's chain. Every method is a
// thin call into broaudio::Engine.

#include "host_audio_internal.h"

#include <vector>

namespace broaudio::api {

void registerAudioContextBusFx(ObjectBuilder& b) {
    // ---- Chorus feedback ---------------------------------------------------
    b.def("setBusChorusFeedback", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusChorusFeedback(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusFeedback", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusFeedback(i32At(a, 0)) : 0.0);
    });

    // ---- EQ ----------------------------------------------------------------
    b.def("setBusEqEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusEqEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusEqEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->getBusEqEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusEqBandGain", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setBusEqBandGain(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusEqBandGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && a.size() >= 2 ? e->getBusEqBandGain(i32At(a, 0), i32At(a, 1)) : 0.0);
    });

    b.def("setBusEqMasterGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusEqMasterGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusEqMasterGain", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusEqMasterGain(i32At(a, 0)) : 0.0);
    });

    // ---- Distortion --------------------------------------------------------
    b.def("setBusDistortionEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDistortionEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusDistortionEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->getBusDistortionEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusDistortionMode", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDistortionMode(i32At(a, 0), parseDistortionMode(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("getBusDistortionMode", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromUtf8(e && !a.empty() ? distortionModeToString(e->getBusDistortionMode(i32At(a, 0))) : "softclip");
    });

    b.def("setBusDistortionDrive", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDistortionDrive(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionDrive", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionDrive(i32At(a, 0)) : 1.0);
    });

    b.def("setBusDistortionMix", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDistortionMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionMix", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionMix(i32At(a, 0)) : 1.0);
    });

    b.def("setBusDistortionOutputGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDistortionOutputGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionOutputGain", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionOutputGain(i32At(a, 0)) : 1.0);
    });

    b.def("setBusDistortionCrushBits", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDistortionCrushBits(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionCrushBits", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionCrushBits(i32At(a, 0)) : 16.0);
    });

    b.def("setBusDistortionCrushRate", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDistortionCrushRate(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDistortionCrushRate", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusDistortionCrushRate(i32At(a, 0)) : 1.0);
    });

    // ---- Effect order ------------------------------------------------------
    // setBusEffectOrder(busId, names[]): 1..7 slot names ("filter", "delay",
    // "compressor", "chorus", "reverb", "equalizer", "distortion"). A longer
    // or empty list is ignored; a name that is not a slot keeps that
    // position's default slot (position i = slot i), so a typo never
    // collapses the chain onto one effect.
    b.def("setBusEffectOrder", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (!e || a.size() < 2 || !ev::isObject(a[1])) return ev::undefined();
        int busId = i32At(a, 0);
        ev::Persistent arr(a[1]);
        Value lenV = ev::getProperty(arr.get(), "length");
        if (ev::isUndefined(lenV) || ev::isObject(lenV)) return ev::undefined();
        double lenD = ev::toDouble(lenV);
        int len = std::isnan(lenD) ? 0 : static_cast<int>(lenD);
        constexpr int kSlots = static_cast<int>(broaudio::EffectSlot::Count);
        if (len <= 0 || len > kSlots) return ev::undefined();

        broaudio::EffectSlot order[kSlots];
        for (int i = 0; i < len; ++i) {
            Value item = ev::getElement(arr.get(), static_cast<uint32_t>(i));
            std::string name = (ev::isUndefined(item) || ev::isObject(item)) ? "" : ev::toUtf8(item);
            order[i] = parseEffectSlot(name, static_cast<broaudio::EffectSlot>(i));
        }
        e->setBusEffectOrder(busId, order, len);
        return ev::undefined();
    });

    // ---- Offline processing ------------------------------------------------
    b.def("processEffectsOffline", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.size() < 2) return ev::null();
        int busId = i32At(a, 0);
        // Float32Array or an ArrayBuffer of float32; any other typed array
        // (or a detached one) is a TypeError. Empty input is null.
        std::vector<float> input;
        if (!readFloatArrayArg(a[1], FloatArrayArg::Float32OrBuffer, "processEffectsOffline: samples", input)) {
            return ev::undefined();
        }
        if (input.empty()) return ev::null();
        std::vector<float> res = e->processEffectsOffline(busId, input.data(), static_cast<int>(input.size()));
        return makeFloat32Array(res);
    });

    // ---- JIT compilation ---------------------------------------------------
    b.def("setBusJitEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusJitEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusJitEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->isBusJitEnabled(i32At(a, 0)) : false);
    });

    b.def("isBusJitEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->isBusJitEnabled(i32At(a, 0)) : false);
    });

    b.def("getBusJitActive", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->isBusJitActive(i32At(a, 0)) : false);
    });

    b.def("isBusJitActive", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->isBusJitActive(i32At(a, 0)) : false);
    });
}

} // namespace broaudio::api
