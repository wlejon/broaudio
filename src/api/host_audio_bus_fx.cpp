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
    b.def("setBusEffectOrder", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2 && ev::isObject(a[1])) {
            int busId = i32At(a, 0);
            Value arr = a[1];
            Value lenV = ev::getProperty(arr, "length");
            if (ev::isNumber(lenV)) {
                int len = static_cast<int>(ev::toDouble(lenV));
                std::vector<broaudio::EffectSlot> slots;
                for (int i = 0; i < len; ++i) {
                    Value item = ev::getProperty(arr, std::to_string(i));
                    if (ev::isString(item)) {
                        slots.push_back(parseEffectSlot(ev::toUtf8(item), broaudio::EffectSlot::Filter));
                    }
                }
                if (!slots.empty()) e->setBusEffectOrder(busId, slots.data(), static_cast<int>(slots.size()));
            }
        }
        return ev::undefined();
    });

    // ---- Offline processing ------------------------------------------------
    b.def("processEffectsOffline", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.size() < 2) return ev::null();
        int busId = i32At(a, 0);
        const uint8_t* rawData = nullptr;
        size_t rawLen = 0;
        size_t elemSize = 1;
        if (!bufferBytes(a[1], &rawData, &rawLen, &elemSize) || rawLen == 0) return ev::null();
        int count = static_cast<int>(rawLen / sizeof(float));
        std::vector<float> res = e->processEffectsOffline(busId, reinterpret_cast<const float*>(rawData), count);
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
