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

    // getClipWaveform(clipId, numBins) -> Float32Array(numBins * 2) of
    // interleaved [min, max] pairs, zero-filled where the engine has nothing
    // (unknown clip, empty clip) so a display can index it unconditionally;
    // undefined for numBins outside 1..1024.
    b.def("getClipWaveform", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.size() < 2) return ev::undefined();
        int clipId = i32At(a, 0);
        int numBins = i32At(a, 1);
        if (numBins <= 0 || numBins > 1024) return ev::undefined();
        std::vector<float> out(static_cast<size_t>(numBins) * 2, 0.0f);
        std::vector<float> wf = e->getClipWaveform(clipId, numBins);
        size_t n = std::min(out.size(), wf.size());
        if (n > 0) std::memcpy(out.data(), wf.data(), n * sizeof(float));
        return makeFloat32Array(out);
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

    // setPlaybackSend(playbackId, sendBusId, amount): aux send from a
    // playback instance, the counterpart of setVoiceSend / setBusSend.
    b.def("setPlaybackSend", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) {
            e->setPlaybackSend(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    // ---- Streams -----------------------------------------------------------
    // createStream(channels = 1, ringFrames = 0) -> playbackId for a live PCM
    // source. ringFrames 0 lets the engine pick (~2 s at the engine rate).
    b.def("createStream", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (!e) return ev::fromDouble(-1);
        int channels = a.size() >= 1 ? i32At(a, 0) : 1;
        int ringFrames = a.size() >= 2 ? i32At(a, 1) : 0;
        return ev::fromDouble(e->createStream(channels, ringFrames));
    });

    // pushStreamSamples(streamId, Float32Array) -> frames written. Samples
    // must be interleaved at the engine sample rate. A non-typed-array
    // second argument is a TypeError, not a silent 0.
    b.def("pushStreamSamples", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.size() < 2) return ev::fromDouble(0);
        int id = i32At(a, 0);
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[1]);
        if (!info || !info.data) return ev::throwTypeError("Expected Float32Array samples");
        int count = static_cast<int>(info.byteLength / sizeof(float));
        return ev::fromDouble(e->pushStreamSamples(id, reinterpret_cast<const float*>(info.data), count));
    });

    b.def("closeStream", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->closeStream(i32At(a, 0));
        return ev::undefined();
    });

    // createStreamFromFile(path, options?) -> playbackId. Disk-streamed
    // playback: the file decodes incrementally on a broaudio worker thread
    // into a ring the mixer consumes. Throws with the decode error on
    // failure. options: { ringFrames, prebufferFrames, loop, gain }, each
    // coerced the way a JS number/boolean would be (a string "4096" counts).
    b.def("createStreamFromFile", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty() || ev::isUndefined(a[0])) {
            return ev::throwTypeError("createStreamFromFile: file path required");
        }
        std::string path = resolveAudioPath(ev::toUtf8(a[0]));
        broaudio::FileStreamOptions opts;
        if (a.size() >= 2 && ev::isObject(a[1])) {
            ev::Persistent opt(a[1]);
            auto numberOpt = [&](const char* key, double& out) {
                Value v = ev::getProperty(opt.get(), key);
                if (ev::isUndefined(v) || ev::isObject(v)) return false;
                double d = ev::toDouble(v);
                if (std::isnan(d)) return false;
                out = d;
                return true;
            };
            double d = 0.0;
            if (numberOpt("ringFrames", d)) opts.ringFrames = static_cast<int>(d);
            if (numberOpt("prebufferFrames", d)) opts.prebufferFrames = static_cast<int>(d);
            if (numberOpt("gain", d)) opts.gain = static_cast<float>(d);
            Value loop = ev::getProperty(opt.get(), "loop");
            if (!ev::isUndefined(loop)) opts.loop = ev::toBool(loop);
        }
        std::string err;
        int id = e->createStreamFromFile(path.c_str(), opts, &err);
        if (id < 0) {
            return ev::throwError("createStreamFromFile: " + (err.empty() ? std::string("failed to open stream") : err));
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
