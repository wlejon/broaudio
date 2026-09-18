// AudioContext playback-instance and stream methods, beside the clip
// create/play core in host_audio_clips.cpp: clip introspection (channels,
// waveform), transport on a playing instance (playing, region, rate,
// position, seek), per-instance spatialization and bus routing, and the two
// stream sources (a ring the app pushes samples into, and a worker-decoded
// file). Every method is a thin call into broaudio::Engine.

#include "host_audio_internal.h"

namespace broaudio::api {

void registerAudioContextPlayback(ObjectBuilder& b) {
    // ---- Clip introspection ------------------------------------------------
    b.def("getClipChannels", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getClipChannels(i32At(a, 0)) : 0);
    });

    b.def("getClipWaveform", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.size() < 2) return ev::null();
        int clipId = i32At(a, 0);
        int numBins = i32At(a, 1);
        if (numBins <= 0 || numBins > 1024) return ev::null();
        std::vector<float> wf = e->getClipWaveform(clipId, numBins);
        if (wf.empty()) return ev::null();
        return makeFloat32Array(wf);
    });

    // ---- Transport on a playback instance ----------------------------------
    b.def("setPlaybackPlaying", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackPlaying(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setPlaybackRegion", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setPlaybackRegion(i32At(a, 0), static_cast<int>(numAt(a, 1)), static_cast<int>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("setPlaybackRate", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackRate(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getPlaybackPosition", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getPlaybackPosition(i32At(a, 0)) : 0);
    });

    b.def("getPlaybackPositionSeconds", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getPlaybackPositionSeconds(i32At(a, 0)) : 0.0);
    });

    b.def("seekPlayback", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->seekPlayback(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    // ---- Spatialization and routing ----------------------------------------
    b.def("setPlaybackSpatialEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackSpatialEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setPlaybackSpatialPosition", 4, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 4) {
            e->setPlaybackSpatialPosition(i32At(a, 0), static_cast<float>(numAt(a, 1)),
                                          static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        }
        return ev::undefined();
    });

    b.def("setPlaybackSpatialVelocity", 4, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 4) {
            e->setPlaybackSpatialVelocity(i32At(a, 0), static_cast<float>(numAt(a, 1)),
                                          static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        }
        return ev::undefined();
    });

    b.def("setPlaybackSpatialRefDistance", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackSpatialRefDistance(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setPlaybackSpatialMaxDistance", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackSpatialMaxDistance(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setPlaybackSpatialRolloff", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackSpatialRolloff(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setPlaybackSpatialDistanceModel", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackSpatialDistanceModel(i32At(a, 0), parseDistanceModel(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("setPlaybackSpatialOcclusion", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackSpatialOcclusion(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getPlaybackDopplerRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getPlaybackDopplerRatio(i32At(a, 0)) : 1.0);
    });

    b.def("setPlaybackBus", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackBus(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    // ---- Streams -----------------------------------------------------------
    b.def("createStream", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (!e) return ev::fromDouble(-1);
        int channels = a.size() >= 1 ? i32At(a, 0) : 1;
        int ringFrames = a.size() >= 2 ? i32At(a, 1) : 44100;
        return ev::fromDouble(e->createStream(channels, ringFrames));
    });

    b.def("pushStreamSamples", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (!e || a.size() < 2) return ev::fromDouble(0);
        int id = i32At(a, 0);
        const uint8_t* rawData = nullptr;
        size_t rawLen = 0;
        size_t elemSize = 1;
        if (!bufferBytes(a[1], &rawData, &rawLen, &elemSize) || rawLen == 0) return ev::fromDouble(0);
        int count = static_cast<int>(rawLen / sizeof(float));
        return ev::fromDouble(e->pushStreamSamples(id, reinterpret_cast<const float*>(rawData), count));
    });

    b.def("closeStream", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->closeStream(i32At(a, 0));
        return ev::undefined();
    });

    b.def("createStreamFromFile", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::throwError("createStreamFromFile: no engine or path");
        std::string path = ev::toUtf8(a[0]);
        broaudio::FileStreamOptions opts;
        if (a.size() >= 2 && ev::isObject(a[1])) {
            Value optVal = a[1];
            Value rf = ev::getProperty(optVal, "ringFrames");
            if (ev::isNumber(rf)) opts.ringFrames = static_cast<int>(ev::toDouble(rf));
            Value loop = ev::getProperty(optVal, "loop");
            if (ev::isBool(loop)) opts.loop = ev::toBool(loop);
        }
        std::string err;
        int id = e->createStreamFromFile(path.c_str(), opts, &err);
        if (id < 0) {
            std::string msg = err.empty() ? ("createStreamFromFile: failed to open " + path) : err;
            return ev::throwError(msg.c_str());
        }
        return ev::fromDouble(id);
    });

    b.def("getStreamStats", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::null();
        broaudio::StreamStats stats = e->getStreamStats(i32At(a, 0));
        if (!stats.valid) return ev::null();
        ObjectBuilder obj;
        obj.set("decodedFrames", ev::fromDouble(stats.decodedFrames));
        obj.set("playedFrames", ev::fromDouble(stats.playedFrames));
        obj.set("bufferedFrames", ev::fromDouble(stats.bufferedFrames));
        obj.set("underrunFrames", ev::fromDouble(stats.underrunFrames));
        obj.set("finished", ev::fromBool(stats.finished));
        return obj.get();
    });
}

} // namespace broaudio::api
