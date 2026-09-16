// AudioContext voice methods beyond the core set in host_audio_synth.cpp:
// the short ADSR spellings the public surface documents (setVoiceAttack /
// Decay / Sustain / Release, beside the *Time / *Level names), unison,
// per-voice spatialization, bus routing and sample-accurate note scheduling.
// Every method is a thin call into broaudio::Engine.

#include "host_audio_internal.h"

namespace broaudio::api {

void registerAudioContextVoiceExt(ObjectBuilder& b) {
    // ---- ADSR, short names -------------------------------------------------
    b.def("setVoiceAttack", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setAttackTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceDecay", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setDecayTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSustain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setSustainLevel(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceRelease", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setReleaseTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    // ---- Unison ------------------------------------------------------------
    b.def("setVoiceUnisonCount", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceUnisonCount(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceUnisonDetune", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceUnisonDetune(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceUnisonStereoWidth", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceUnisonStereoWidth(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    // ---- Spatialization ----------------------------------------------------
    b.def("setVoiceSpatialEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceSpatialEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceSpatialPosition", 4, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 4) {
            e->setVoiceSpatialPosition(i32At(a, 0), static_cast<float>(numAt(a, 1)),
                                       static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        }
        return ev::undefined();
    });

    b.def("setVoiceSpatialVelocity", 4, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 4) {
            e->setVoiceSpatialVelocity(i32At(a, 0), static_cast<float>(numAt(a, 1)),
                                       static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        }
        return ev::undefined();
    });

    b.def("setVoiceSpatialRefDistance", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceSpatialRefDistance(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSpatialMaxDistance", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceSpatialMaxDistance(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSpatialRolloff", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceSpatialRolloff(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSpatialDistanceModel", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceSpatialDistanceModel(i32At(a, 0), parseDistanceModel(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("getVoiceDopplerRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getVoiceDopplerRatio(i32At(a, 0)) : 1.0);
    });

    // ---- Routing and scheduling --------------------------------------------
    b.def("setVoiceBus", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceBus(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceSend", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setVoiceSend(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("scheduleNoteOn", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->scheduleNoteOn(i32At(a, 0), numAt(a, 1));
        return ev::undefined();
    });

    b.def("scheduleNoteOff", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->scheduleNoteOff(i32At(a, 0), numAt(a, 1));
        return ev::undefined();
    });
}

} // namespace broaudio::api
