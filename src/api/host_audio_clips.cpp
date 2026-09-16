#include "host_audio_internal.h"

namespace broaudio::api {

void registerAudioContextClips(ObjectBuilder& b) {
    b.def("createClip", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::fromDouble(-1);

        Value first = a[0];
        if (auto* hostBuf = hostAudioBufferOf(first)) {
            int channels = hostBuf->numberOfChannels;
            int frames = hostBuf->length;
            if (frames <= 0 || channels <= 0) return ev::fromDouble(-1);

            std::vector<std::vector<float>> chData(channels);
            for (int c = 0; c < channels; ++c) {
                chData[c].resize(frames, 0.0f);
                std::string key = "_ch" + std::to_string(c);
                Value arr = ev::getProperty(first, key);
                if (ev::isTypedArray(arr)) {
                    ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
                    if (info && info.data) {
                        size_t count = std::min(static_cast<size_t>(frames), static_cast<size_t>(info.elementCount));
                        std::memcpy(chData[c].data(), info.data, count * sizeof(float));
                    }
                } else if (c < static_cast<int>(hostBuf->channels.size())) {
                    chData[c] = hostBuf->channels[c];
                }
            }

            std::vector<float> interleaved(frames * channels);
            for (int f = 0; f < frames; ++f) {
                for (int c = 0; c < channels; ++c) {
                    interleaved[f * channels + c] = chData[c][f];
                }
            }

            int clipId = e->createClip(interleaved.data(), frames * channels, channels);
            return ev::fromDouble(clipId);
        }

        const uint8_t* rawData = nullptr;
        size_t rawLen = 0, elemSize = 1;
        if (!bufferBytes(first, &rawData, &rawLen, &elemSize) || rawLen == 0) {
            return ev::throwTypeError("createClip: expected AudioBuffer or Float32Array");
        }

        int numSamples = static_cast<int>(rawLen / sizeof(float));
        int channels = a.size() >= 2 ? i32At(a, 1) : 1;
        if (channels <= 0) channels = 1;
        const float* samples = reinterpret_cast<const float*>(rawData);

        int clipId = e->createClip(samples, numSamples, channels);
        return ev::fromDouble(clipId);
    });

    b.def("deleteClip", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->deleteClip(i32At(a, 0));
        return ev::undefined();
    });

    b.def("getClipSampleCount", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getClipSampleCount(i32At(a, 0)) : 0);
    });

    b.def("playClip", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::fromDouble(-1);
        int clipId = i32At(a, 0);
        float gain = a.size() >= 2 ? static_cast<float>(numAt(a, 1)) : 1.0f;
        bool loop = a.size() >= 3 ? boolAt(a, 2) : false;
        return ev::fromDouble(e->playClip(clipId, gain, loop));
    });

    b.def("playClipAt", 4, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (!e || a.size() < 2) return ev::fromDouble(-1);
        int clipId = i32At(a, 0);
        double when = numAt(a, 1);
        float gain = a.size() >= 3 ? static_cast<float>(numAt(a, 2)) : 1.0f;
        bool loop = a.size() >= 4 ? boolAt(a, 3) : false;
        return ev::fromDouble(e->playClipAt(clipId, when, gain, loop));
    });

    b.def("stopClip", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->stopPlayback(i32At(a, 0));
        return ev::undefined();
    });

    b.def("isClipPlaying", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? (e->getPlaybackPosition(i32At(a, 0)) > 0.0f) : false);
    });

    b.def("setClipGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setClipPan", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackPan(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setClipLoop", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackLoop(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("stopPlayback", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->stopPlayback(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setPlaybackGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setPlaybackPan", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackPan(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setPlaybackLoop", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setPlaybackLoop(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("createClipFromFile", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::fromDouble(-1);
        std::string path = ev::toUtf8(a[0]);
        return ev::fromDouble(e->createClipFromFile(path.c_str()));
    });

    b.def("createClipFromFileAsync", 1, [](Value, std::span<const Value> a) -> Value {
        ev::Persistent p{ev::createPromise()};
        auto* e = getAudioEngine();
        if (!e || a.empty()) {
            ev::rejectPromise(p.get(), hostMakeDomError("Error", "createClipFromFileAsync: no engine or path"));
            return p.get();
        }
        std::string path = ev::toUtf8(a[0]);
        // The Ex form hands back the decoder's own message (which codec,
        // what went wrong); the rejection carries it so the caller can act.
        std::string err;
        int clipId = e->createClipFromFileEx(path.c_str(), &err);
        if (clipId >= 0) {
            ev::resolvePromise(p.get(), ev::fromDouble(clipId));
        } else {
            std::string msg = err.empty() ? "createClipFromFileAsync: failed to load file" : err;
            ev::rejectPromise(p.get(), hostMakeDomError("Error", msg));
        }
        return p.get();
    });
}

} // namespace broaudio::api
