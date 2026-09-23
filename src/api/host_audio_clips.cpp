#include "host_audio_internal.h"

namespace broaudio::api {

void registerAudioContextClips(ObjectBuilder& b) {
    b.def("createClip", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::fromDouble(-1);

        // a[0] is read from the rooted span each time: the per-channel
        // getProperty below may move it.
        if (auto* hostBuf = hostAudioBufferOf(a[0])) {
            int channels = hostBuf->numberOfChannels;
            int frames = hostBuf->length;
            if (frames <= 0 || channels <= 0) return ev::fromDouble(-1);

            std::vector<std::vector<float>> chData(channels);
            for (int c = 0; c < channels; ++c) {
                chData[c].resize(frames, 0.0f);
                std::string key = "_ch" + std::to_string(c);
                Value arr = ev::getProperty(a[0], key);
                if (isFloat32Array(arr)) {
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

        // Interleaved float32 samples: a Float32Array, or an ArrayBuffer read
        // as float32. Any other typed array is a TypeError, not bytes
        // reinterpreted as floats.
        std::vector<float> input;
        if (!(ev::isTypedArray(a[0]) || ev::isArrayBuffer(a[0]))) {
            return ev::throwTypeError("createClip: expected AudioBuffer or Float32Array");
        }
        if (!readFloatArrayArg(a[0], FloatArrayArg::Float32OrBuffer, "createClip: samples", input)) {
            return ev::undefined();
        }
        if (input.empty()) return ev::throwTypeError("createClip: expected AudioBuffer or Float32Array");

        int numSamples = static_cast<int>(input.size());
        int channels = a.size() >= 2 ? i32At(a, 1) : 1;
        if (channels <= 0) channels = 1;
        const float* samples = input.data();

        // Optional 3rd arg: the PCM's source sample rate. When it differs
        // from the engine rate, resample so the clip plays at the right
        // pitch/speed (mirrors decodeAudioData). Without it, the samples are
        // assumed to be at engine rate.
        std::vector<float> resampled;
        if (a.size() >= 3 && ev::isNumber(a[2])) {
            int srcRate = i32At(a, 2);
            const int engRate = e->sampleRate();
            if (srcRate > 0 && srcRate != engRate) {
                resampled = broaudio::resample(samples, numSamples / channels,
                                               channels, srcRate, engRate);
                if (!resampled.empty()) {
                    samples = resampled.data();
                    numSamples = static_cast<int>(resampled.size());
                }
            }
        }

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

    // playClip(clipId, gain?, loop?, when?) -> playbackId. A numeric 4th arg
    // is a sample-accurate start time (engine seconds, from ctx.currentTime):
    // the clip is queued on the audio clock so streamed chunks join
    // gaplessly — no main-thread setTimeout jitter or clock drift. A `when`
    // at/before now plays immediately, same as the 3-arg form.
    b.def("playClip", 4, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::fromDouble(-1);
        int clipId = i32At(a, 0);
        float gain = a.size() >= 2 ? static_cast<float>(numAt(a, 1)) : 1.0f;
        bool loop = a.size() >= 3 ? boolAt(a, 2) : false;
        if (a.size() >= 4 && ev::isNumber(a[3])) {
            return ev::fromDouble(e->playClipAt(clipId, numAt(a, 3), gain, loop));
        }
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

    // True while the playback is producing audio: started, its `when`
    // reached, not paused, not finished or stopped. Answered from the
    // playback's own state (Engine::getPlaybackState), so a playback at
    // position 0 counts and a finished one parked at its end does not.
    b.def("isClipPlaying", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() && e->isPlaybackPlaying(i32At(a, 0)));
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
        std::string path = resolveAudioPath(ev::toUtf8(a[0]));
        return ev::fromDouble(e->createClipFromFile(path.c_str()));
    });

    // createClipFromFileAsync(path) -> Promise<clipId>. Decode + resample run
    // on a background thread (host_audio_io.cpp); the promise resolves with
    // the clip id or rejects with an Error carrying the actionable decode
    // message from broaudio (corrupt stream, size cap, unsupported codec)
    // once the host ticks (api.h tickAsyncJobs / drainMicChunks).
    b.def("createClipFromFileAsync", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || ev::isUndefined(a[0])) {
            return ev::throwTypeError("createClipFromFileAsync: file path required");
        }
        // Resolve on the JS thread — the worker has no notion of the app
        // directory or the mount table.
        std::string path = resolveAudioPath(ev::toUtf8(a[0]));
        return launchClipLoad(path);
    });
}

} // namespace broaudio::api
