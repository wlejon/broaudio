#include "host_audio_internal.h"
#include <broaudio/dsp/resampler.h>
#include <broaudio/io/audio_file.h>

namespace broaudio::api {

HostClass g_audioContextClass;

void hostAudioContextDtor(void* p) {
    delete static_cast<HostAudioContext*>(p);
}

HostAudioContext* hostAudioContextOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioContext*>(g_audioContextClass.unwrap(v));
    if (!p || p->tag != kHostAudioContextTag) return nullptr;
    return p;
}

Value makeAudioContextValue() {
    auto* ctx = new HostAudioContext();
    ctx->state = "running";
    auto* e = getAudioEngine();
    if (e) e->setMasterPaused(false);
    ObjectBuilder b(g_audioContextClass.make(ctx, hostAudioContextDtor));

    b.set("destination", makeDestinationNodeValue());
    b.set("listener", makeListenerValue());

    return b.get();
}

void decorateAudioContextProto(ObjectBuilder& b) {
    // 1. Properties
    b.accessor("currentTime", [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e ? e->currentTime() : 0.0);
    }, nullptr);

    b.accessor("sampleRate", [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e ? e->sampleRate() : 44100);
    }, nullptr);

    b.accessor("state", [](Value self_, std::span<const Value>) {
        HostAudioContext* ctx = hostAudioContextOf(self_);
        return ev::fromUtf8(ctx ? ctx->state : "running");
    }, nullptr);

    b.accessor("outputLatency", [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e ? e->outputLatencySeconds() : 0.0);
    }, nullptr);

    b.accessor("baseLatency", [](Value, std::span<const Value>) {
        return ev::fromDouble(0.0);
    }, nullptr);

    b.accessor("masterGain",
        [](Value, std::span<const Value>) {
            auto* e = getAudioEngine();
            return ev::fromDouble(e ? e->masterGain() : 1.0);
        },
        [](Value, std::span<const Value> a) {
            auto* e = getAudioEngine();
            if (e && !a.empty()) e->setMasterGain(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });

    b.accessor("recording", [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        return ev::fromBool(e ? e->isRecording() : false);
    }, nullptr);

    b.accessor("dopplerFactor",
        [](Value, std::span<const Value>) {
            auto* e = getAudioEngine();
            return ev::fromDouble(e ? e->dopplerFactor() : 1.0);
        },
        [](Value, std::span<const Value> a) {
            auto* e = getAudioEngine();
            if (e && !a.empty()) e->setDopplerFactor(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });

    b.accessor("micMuted",
        [](Value, std::span<const Value>) {
            auto* e = getAudioEngine();
            return ev::fromBool(e ? e->isMicMuted() : false);
        },
        [](Value, std::span<const Value> a) {
            auto* e = getAudioEngine();
            if (e && !a.empty()) e->setMicMuted(boolAt(a, 0));
            return ev::undefined();
        });

    b.accessor("micMonitorGain",
        [](Value, std::span<const Value>) {
            auto* e = getAudioEngine();
            return ev::fromDouble(e ? e->micMonitorGain() : 0.0);
        },
        [](Value, std::span<const Value> a) {
            auto* e = getAudioEngine();
            if (e && !a.empty()) e->setMicMonitorGain(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });

    b.accessor("micBus",
        [](Value, std::span<const Value>) {
            auto* e = getAudioEngine();
            return ev::fromDouble(e ? e->micBus() : 0);
        },
        [](Value, std::span<const Value> a) {
            auto* e = getAudioEngine();
            if (e && !a.empty()) e->setMicBus(i32At(a, 0));
            return ev::undefined();
        });

    // 2. Lifecycle
    b.def("suspend", 0, [](Value self_, std::span<const Value>) {
        HostAudioContext* ctx = hostAudioContextOf(self_);
        if (ctx && ctx->state != "closed") {
            ctx->state = "suspended";
            auto* e = getAudioEngine();
            if (e) e->setMasterPaused(true);
        }
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("resume", 0, [](Value self_, std::span<const Value>) {
        HostAudioContext* ctx = hostAudioContextOf(self_);
        if (ctx && ctx->state != "closed") {
            ctx->state = "running";
            auto* e = getAudioEngine();
            if (e) e->setMasterPaused(false);
        }
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("close", 0, [](Value self_, std::span<const Value>) {
        HostAudioContext* ctx = hostAudioContextOf(self_);
        if (ctx) {
            ctx->state = "closed";
            auto* e = getAudioEngine();
            if (e) {
                for (int vid : ctx->voiceIds) {
                    e->stopVoice(vid, 0.0);
                    e->removeVoice(vid);
                }
                ctx->voiceIds.clear();
                e->stopMicCapture();
                e->stopRecording();
                e->setMasterPaused(true);
            }
        }
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    // 3. Node factories
    b.def("createGain", 0, [](Value, std::span<const Value>) {
        return makeGainNodeValue();
    });

    b.def("createOscillator", 0, [](Value self_, std::span<const Value>) {
        // The receiver is a plain copy: unwrap it before the node allocates.
        HostAudioContext* ctx = hostAudioContextOf(self_);
        Value oscVal = makeOscillatorNodeValue();
        if (ctx) {
            HostOscillatorNode* osc = oscOf(oscVal);
            if (osc && osc->voiceId >= 0) {
                ctx->voiceIds.push_back(osc->voiceId);
            }
        }
        return oscVal;
    });

    b.def("createPeriodicWave", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::null();
        // The options read may run a getter and move the heap, so it goes
        // first and the arrays' bytes are copied out after it.
        bool disableNorm = false;
        if (a.size() >= 3 && ev::isObject(a[2])) {
            Value dn = ev::getProperty(a[2], "disableNormalization");
            if (ev::isBool(dn)) disableNorm = ev::toBool(dn);
        }
        std::vector<float> real, imag;
        if (!readFloatArrayArg(a[0], FloatArrayArg::Float32OrPlain, "createPeriodicWave: real", real) ||
            !readFloatArrayArg(a[1], FloatArrayArg::Float32OrPlain, "createPeriodicWave: imag", imag)) {
            return ev::undefined();
        }
        // Web Audio requires equal lengths; the shorter half is zero-padded
        // instead, as `new PeriodicWave` does.
        size_t count = std::max(real.size(), imag.size());
        real.resize(count, 0.0f);
        imag.resize(count, 0.0f);
        return makePeriodicWaveValue(real.data(), imag.data(), static_cast<int>(count), disableNorm);
    });

    b.def("createBiquadFilter", 0, [](Value, std::span<const Value>) {
        return makeBiquadFilterNodeValue();
    });

    b.def("createAnalyser", 0, [](Value, std::span<const Value>) {
        return makeAnalyserNodeValue();
    });

    b.def("createBufferSource", 0, [](Value, std::span<const Value>) {
        return makeAudioBufferSourceNodeValue();
    });

    b.def("createBuffer", 3, [](Value, std::span<const Value> a) {
        int ch = a.size() >= 1 ? i32At(a, 0) : 1;
        int len = a.size() >= 2 ? i32At(a, 1) : 0;
        // A buffer plays at its own rate, so a missing rate is the context's
        // (no resampling), not a fixed 44100.
        auto* e = getAudioEngine();
        int sr = hasArg(a, 2) ? i32At(a, 2) : (e ? e->sampleRate() : 44100);
        return makeAudioBufferValue(ch, len, sr);
    });

    b.def("createPanner", 0, [](Value, std::span<const Value>) {
        return makePannerNodeValue();
    });

    b.def("createStereoPanner", 0, [](Value, std::span<const Value>) {
        return makeStereoPannerNodeValue();
    });

    b.def("createDelay", 1, [](Value, std::span<const Value> a) {
        double maxTime = !a.empty() ? numAt(a, 0) : 1.0;
        return makeDelayNodeValue(maxTime);
    });

    b.def("createDynamicsCompressor", 0, [](Value, std::span<const Value>) {
        return makeDynamicsCompressorNodeValue();
    });

    b.def("createWaveShaper", 0, [](Value, std::span<const Value>) {
        return makeWaveShaperNodeValue();
    });

    b.def("createConvolver", 0, [](Value, std::span<const Value>) {
        return makeConvolverNodeValue();
    });

    b.def("createChannelSplitter", 1, [](Value, std::span<const Value> a) {
        int outputs = !a.empty() ? i32At(a, 0) : 6;
        return makeChannelSplitterNodeValue(outputs);
    });

    b.def("createChannelMerger", 1, [](Value, std::span<const Value> a) {
        int inputs = !a.empty() ? i32At(a, 0) : 6;
        return makeChannelMergerNodeValue(inputs);
    });

    b.def("createMediaStreamSource", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        if (!ev::isObject(a[0]) || !hostMediaStreamOf(a[0])) {
            return ev::throwTypeError("Expected MediaStream argument");
        }
        return makeMediaStreamAudioSourceNodeValue();
    });

    b.def("createVoiceAllocator", 1, [](Value, std::span<const Value> a) {
        return makeVoiceAllocatorValue(a.empty() ? 16 : i32At(a, 0));
    });

    b.def("createModMatrix", 0, [](Value, std::span<const Value>) {
        return makeModMatrixValue();
    });

    b.def("createMidiInput", 0, [](Value, std::span<const Value>) {
        return makeMidiInputValue();
    });

    b.def("createSequence", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        HostVoiceAllocator* va = ev::isObject(a[0]) ? hostVoiceAllocatorOf(a[0]) : nullptr;
        if (!va) return ev::throwTypeError("Expected VoiceAllocator argument");
        return makeSequenceValue(a[0]);
    });

    // 4. decodeAudioData
    b.def("decodeAudioData", 3, [](Value, std::span<const Value> a) -> Value {
        // The callbacks are read from `a` (rooted) at each use, never from a
        // local copy: every call below allocates.
        const bool hasSuccessCb = a.size() >= 2 && ev::isFunction(a[1]);
        const bool hasErrorCb = a.size() >= 3 && ev::isFunction(a[2]);
        ev::Persistent p(ev::createPromise());
        auto fail = [&](const std::string& msg) -> Value {
            ev::Persistent err(hostMakeDomError("EncodingError", msg));
            if (hasErrorCb) {
                const Value arg = err.get();
                ev::call(a[2], ev::undefined(), std::span<const Value>(&arg, 1));
            }
            ev::rejectPromise(p.get(), err.get());
            return p.get();
        };
        if (a.empty()) {
            ev::Persistent err(hostMakeDomError("TypeError", "decodeAudioData: audio buffer argument required"));
            ev::rejectPromise(p.get(), err.get());
            return p.get();
        }

        const uint8_t* rawData = nullptr;
        size_t rawLen = 0, elemSize = 1;
        if (!bufferBytes(a[0], &rawData, &rawLen, &elemSize) || rawLen == 0) {
            if (!hasSuccessCb && !hasErrorCb) return ev::null();
            return fail("decodeAudioData: invalid buffer");
        }

        // rawData points into the moving heap; the decode consumes it before
        // anything allocates.
        broaudio::AudioFileData data = broaudio::loadAudioFileFromMemory(rawData, rawLen);
        if (!data.valid()) {
            if (!hasSuccessCb && !hasErrorCb) return ev::null();
            return fail(data.error.empty() ? "decodeAudioData: failed to decode audio" : data.error);
        }

        auto* e = getAudioEngine();
        int engRate = e ? e->sampleRate() : 44100;
        std::vector<float> samples;
        int numFrames = data.numFrames;
        if (data.sampleRate > 0 && data.sampleRate != engRate && data.channels > 0) {
            samples = broaudio::resample(data.samples.data(), data.numFrames, data.channels, data.sampleRate, engRate);
            numFrames = static_cast<int>(samples.size() / data.channels);
        } else {
            samples = std::move(data.samples);
        }

        ev::Persistent bufVal(makeAudioBufferValue(data.channels, numFrames, engRate));
        HostAudioBuffer* hostBuf = hostAudioBufferOf(bufVal.get());
        if (hostBuf && numFrames > 0 && data.channels > 0) {
            hostBuf->channels.resize(data.channels, std::vector<float>(numFrames, 0.0f));
            for (int c = 0; c < data.channels; ++c) {
                for (int i = 0; i < numFrames; ++i) {
                    hostBuf->channels[c][i] = samples[i * data.channels + c];
                }
            }
        }

        ev::Persistent samplesArr(makeFloat32Array(samples));
        // The AudioDecodedBuffer fields, on the AudioBuffer and (for the
        // synchronous `ctx.decodeAudioData(bytes).samples` form) on the
        // promise itself.
        auto decorate = [&](ev::Persistent& target) {
            target.set(ev::setProperty(target.get(), "samples", samplesArr.get()));
            target.set(ev::setProperty(target.get(), "channels", ev::fromDouble(data.channels)));
            target.set(ev::setProperty(target.get(), "sampleRate", ev::fromDouble(engRate)));
            target.set(ev::setProperty(target.get(), "numFrames", ev::fromDouble(numFrames)));
        };
        decorate(bufVal);
        decorate(p);

        if (hasSuccessCb) {
            const Value arg = bufVal.get();
            ev::call(a[1], ev::undefined(), std::span<const Value>(&arg, 1));
        }
        ev::resolvePromise(p.get(), bufVal.get());
        return p.get();
    });

    b.def("decodeAudioFile", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        std::string path = resolveAudioPath(ev::toUtf8(a[0]));
        broaudio::AudioFileData data = broaudio::loadAudioFile(path.c_str());
        if (!data.valid()) return ev::null();

        auto* e = getAudioEngine();
        int engRate = e ? e->sampleRate() : 44100;
        std::vector<float> samples;
        int numFrames = data.numFrames;
        if (data.sampleRate > 0 && data.sampleRate != engRate && data.channels > 0) {
            samples = broaudio::resample(data.samples.data(), data.numFrames, data.channels, data.sampleRate, engRate);
            numFrames = static_cast<int>(samples.size() / data.channels);
        } else {
            samples = std::move(data.samples);
        }

        ObjectBuilder res;
        Value samplesArr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(samples.size()));
        ev::fillTypedArray(samplesArr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(samples.data()),
                                                                samples.size() * sizeof(float)));
        res.set("samples", samplesArr);
        res.set("channels", ev::fromDouble(data.channels));
        res.set("sampleRate", ev::fromDouble(engRate));
        res.set("numFrames", ev::fromDouble(numFrames));
        return res.get();
    });

    // 5. Recording & WAV
    b.def("startRecording", 0, [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        if (e) e->startRecording();
        return ev::undefined();
    });

    b.def("stopRecording", 0, [](Value, std::span<const Value>) -> Value {
        auto* e = getAudioEngine();
        if (!e) return ev::null();
        e->stopRecording();
        std::vector<float> rec = e->getRecordBuffer();
        if (rec.empty()) return ev::null();
        Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(rec.size()));
        ev::fillTypedArray(arr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(rec.data()),
                                                        rec.size() * sizeof(float)));
        return arr;
    });

    b.def("exportRecordingToWav", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (!e || a.empty()) return ev::fromBool(false);
        std::string path = resolveAudioWritePath(ev::toUtf8(a[0]));
        return ev::fromBool(e->exportRecordingToWav(path.c_str()));
    });

    // saveWav(path, Float32Array samples, channels, sampleRate) -> bool. The
    // samples must be a typed array (a TypeError otherwise); the path goes
    // through the host's resolver like every other file the context writes.
    b.def("saveWav", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::fromBool(false);
        std::string path = resolveAudioWritePath(ev::toUtf8(a[0]));
        std::vector<float> samples;
        if (!readFloatArrayArg(a[1], FloatArrayArg::Float32Only, "saveWav: samples", samples)) {
            return ev::undefined();
        }
        int channels = i32At(a, 2);
        int sampleRate = i32At(a, 3);
        if (channels <= 0 || sampleRate <= 0) return ev::fromBool(false);
        int frames = static_cast<int>(samples.size()) / channels;
        return ev::fromBool(broaudio::saveWav(path.c_str(), samples.data(),
                                              frames, channels, sampleRate));
    });

    // 6. Master effect shortcuts
    b.def("allocateFilterSlot", 0, [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e ? e->allocateFilterSlot() : -1);
    });

    b.def("releaseFilterSlot", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->releaseFilterSlot(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setFilterEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setFilterEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setFilterType", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setFilterType(i32At(a, 0), parseFilterType(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("setFilterFrequency", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setFilterFrequency(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setFilterQ", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setFilterQ(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setFilterGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setFilterGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setDelayEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setDelayEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setDelayTime", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setDelayTime(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setDelayFeedback", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setDelayFeedback(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setDelayMix", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setDelayMix(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setReverbEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusReverbEnabled(0, boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setReverbRoomSize", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusReverbRoomSize(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setReverbDamping", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusReverbDamping(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setReverbMix", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusReverbMix(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    // Master chorus and compressor: the bus-0 effects under the short names
    // the delay and reverb shortcuts above already use.
    b.def("setChorusEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusChorusEnabled(0, boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setChorusRate", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusChorusRate(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setChorusDepth", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusChorusDepth(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setChorusMix", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusChorusMix(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setChorusFeedback", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusChorusFeedback(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setChorusBaseDelay", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusChorusBaseDelay(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setCompressorEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusCompressorEnabled(0, boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setCompressorThreshold", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusCompressorThreshold(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setCompressorRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusCompressorRatio(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setCompressorAttack", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusCompressorAttack(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setCompressorRelease", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setBusCompressorRelease(0, static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setLimiterEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setLimiterEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setLimiterThreshold", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setLimiterThreshold(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setLimiterRelease", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setLimiterRelease(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    // 7. Spatial shortcuts
    b.def("setListenerPosition", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) {
            e->setListenerPosition(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setListenerOrientation", 6, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 6) {
            e->setListenerOrientation(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)),
                                     static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
        }
        return ev::undefined();
    });

    b.def("setListenerVelocity", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) {
            e->setListenerVelocity(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setHeadModelEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setHeadModelEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.def("setHeadModelIldStrength", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setHeadModelIldStrength(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setHeadModelBehindAttenuation", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setHeadModelBehindAttenuation(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setHeadModelNearCutoff", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setHeadModelNearCutoff(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setHeadModelFarCutoffRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->setHeadModelFarCutoffRatio(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setHeadModelElevation", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setHeadModelElevation(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setHeadModelCutoffRange", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setHeadModelCutoffRange(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    // 8. Modular AudioContext methods
    registerAudioContextClips(b);
    registerAudioContextPlayback(b);
    registerAudioContextVoice(b);
    registerAudioContextVoiceExt(b);
    registerAudioContextBuses(b);
    registerAudioContextBusFx(b);
    registerAudioContextSynthExt(b);
    registerAudioContextPresets(b);
}

void installAudioGlobals() {
    // 1. AudioContext and webkitAudioContext
    g_audioContextClass.install(
        "AudioContext", 0,
        [](Value, std::span<const Value>) { return makeAudioContextValue(); },
        decorateAudioContextProto);
    g_audioContextClass.alias("webkitAudioContext");

    // 2. AudioNode & AudioParam
    g_audioNodeClass.install("AudioNode", 0, nullptr, decorateAudioNodeProto);
    g_audioParamClass.install("AudioParam", 0, nullptr, decorateAudioParamProto);

    // 2b. AudioDestinationNode (`ctx.destination`; not constructible from JS)
    g_audioDestinationNodeClass.install("AudioDestinationNode", 0, nullptr,
                                        decorateAudioDestinationNodeProto);
    g_audioDestinationNodeClass.inherit(g_audioNodeClass);

    // 3. GainNode
    g_gainNodeClass.install("GainNode", 0,
        [](Value, std::span<const Value>) { return makeGainNodeValue(); },
        nullptr);
    g_gainNodeClass.inherit(g_audioNodeClass);

    // 4. OscillatorNode
    g_oscillatorNodeClass.install("OscillatorNode", 0,
        [](Value, std::span<const Value>) { return makeOscillatorNodeValue(); },
        decorateOscillatorNodeProto);
    g_oscillatorNodeClass.inherit(g_audioNodeClass);

    // 5. AudioBuffer
    g_audioBufferClass.install(
        "AudioBuffer", 1,
        [](Value, std::span<const Value> a) -> Value {
            auto* eng = getAudioEngine();
            int length = 0, channels = 1, sampleRate = eng ? eng->sampleRate() : 44100;
            if (!a.empty() && ev::isObject(a[0])) {
                // Read the options off a[0] (rooted) each time: a getProperty
                // may move the object.
                Value lenV = ev::getProperty(a[0], "length");
                if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) length = static_cast<int>(ev::toDouble(lenV));
                Value chV = ev::getProperty(a[0], "numberOfChannels");
                if (!ev::isUndefined(chV) && !ev::isObject(chV)) channels = static_cast<int>(ev::toDouble(chV));
                Value srV = ev::getProperty(a[0], "sampleRate");
                if (!ev::isUndefined(srV) && !ev::isObject(srV)) sampleRate = static_cast<int>(ev::toDouble(srV));
            }
            if (length <= 0) return ev::throwTypeError("AudioBuffer: length must be positive");
            return makeAudioBufferValue(channels, length, sampleRate);
        },
        decorateAudioBufferProto);

    // 6. AudioBufferSourceNode
    g_audioBufferSourceNodeClass.install("AudioBufferSourceNode", 0,
        [](Value, std::span<const Value>) { return makeAudioBufferSourceNodeValue(); },
        decorateAudioBufferSourceNodeProto);
    g_audioBufferSourceNodeClass.inherit(g_audioNodeClass);

    // 7. BiquadFilterNode
    g_biquadFilterNodeClass.install("BiquadFilterNode", 0,
        [](Value, std::span<const Value>) { return makeBiquadFilterNodeValue(); },
        decorateBiquadFilterNodeProto);
    g_biquadFilterNodeClass.inherit(g_audioNodeClass);

    // 8. AnalyserNode
    g_analyserNodeClass.install("AnalyserNode", 0,
        [](Value, std::span<const Value>) { return makeAnalyserNodeValue(); },
        decorateAnalyserNodeProto);
    g_analyserNodeClass.inherit(g_audioNodeClass);

    // 9. PannerNode & StereoPannerNode
    g_pannerNodeClass.install("PannerNode", 0,
        [](Value, std::span<const Value>) { return makePannerNodeValue(); },
        decoratePannerNodeProto);
    g_pannerNodeClass.inherit(g_audioNodeClass);

    g_stereoPannerNodeClass.install("StereoPannerNode", 0,
        [](Value, std::span<const Value>) { return makeStereoPannerNodeValue(); },
        decorateStereoPannerNodeProto);
    g_stereoPannerNodeClass.inherit(g_audioNodeClass);

    // 10. DelayNode
    g_delayNodeClass.install("DelayNode", 0,
        [](Value, std::span<const Value> a) {
            double maxDelay = a.empty() ? 1.0 : numAt(a, 0);
            return makeDelayNodeValue(maxDelay);
        },
        decorateDelayNodeProto);
    g_delayNodeClass.inherit(g_audioNodeClass);

    // 11. DynamicsCompressorNode
    g_dynamicsCompressorNodeClass.install("DynamicsCompressorNode", 0,
        [](Value, std::span<const Value>) { return makeDynamicsCompressorNodeValue(); },
        decorateDynamicsCompressorNodeProto);
    g_dynamicsCompressorNodeClass.inherit(g_audioNodeClass);

    // 12. WaveShaperNode
    g_waveShaperNodeClass.install("WaveShaperNode", 0,
        [](Value, std::span<const Value>) { return makeWaveShaperNodeValue(); },
        decorateWaveShaperNodeProto);
    g_waveShaperNodeClass.inherit(g_audioNodeClass);

    // 13. ConvolverNode
    g_convolverNodeClass.install("ConvolverNode", 0,
        [](Value, std::span<const Value>) { return makeConvolverNodeValue(); },
        decorateConvolverNodeProto);
    g_convolverNodeClass.inherit(g_audioNodeClass);

    // 14. ChannelSplitterNode & ChannelMergerNode
    g_channelSplitterNodeClass.install("ChannelSplitterNode", 0,
        [](Value, std::span<const Value> a) {
            int outputs = a.empty() ? 6 : i32At(a, 0);
            return makeChannelSplitterNodeValue(outputs);
        },
        decorateChannelSplitterNodeProto);
    g_channelSplitterNodeClass.inherit(g_audioNodeClass);

    g_channelMergerNodeClass.install("ChannelMergerNode", 0,
        [](Value, std::span<const Value> a) {
            int inputs = a.empty() ? 6 : i32At(a, 0);
            return makeChannelMergerNodeValue(inputs);
        },
        decorateChannelMergerNodeProto);
    g_channelMergerNodeClass.inherit(g_audioNodeClass);

    // 15. PeriodicWave
    g_periodicWaveClass.install("PeriodicWave", 0,
        [](Value, std::span<const Value> a) {
            // Two forms: Web Audio's `new PeriodicWave(ctx, {real, imag,
            // disableNormalization})`, and the positional
            // `new PeriodicWave(real, imag, {disableNormalization})`.
            // The halves are copied out, so a later property read (which may
            // allocate) cannot leave them stale. A missing half (or undefined
            // / null) is empty; any other non-array is a TypeError.
            std::vector<float> rStorage, iStorage;
            auto half = [](Value v, const char* what, std::vector<float>& out) {
                if (ev::isUndefined(v) || ev::isNull(v)) return true;
                return readFloatArrayArg(v, FloatArrayArg::Float32OrPlain, what, out);
            };
            ev::Persistent options;
            if (!a.empty() && hostAudioContextOf(a[0])) {
                if (a.size() >= 2 && ev::isObject(a[1])) {
                    options.set(a[1]);
                    ev::Persistent real(ev::getProperty(options.get(), "real"));
                    if (!half(real.get(), "PeriodicWave: options.real", rStorage)) return ev::undefined();
                    ev::Persistent imag(ev::getProperty(options.get(), "imag"));
                    if (!half(imag.get(), "PeriodicWave: options.imag", iStorage)) return ev::undefined();
                }
            } else {
                if (!half(a.empty() ? ev::undefined() : a[0], "PeriodicWave: real", rStorage) ||
                    !half(a.size() >= 2 ? a[1] : ev::undefined(), "PeriodicWave: imag", iStorage)) {
                    return ev::undefined();
                }
                if (a.size() >= 3 && ev::isObject(a[2])) options.set(a[2]);
            }
            const size_t rCount = rStorage.size(), iCount = iStorage.size();
            bool disableNorm = false;
            if (ev::isObject(options.get())) {
                Value dn = ev::getProperty(options.get(), "disableNormalization");
                if (!ev::isUndefined(dn)) disableNorm = ev::toBool(dn);
            }
            // A shorter (or missing) half is zero-padded to the longer one;
            // makePeriodicWaveValue reads `count` values from each.
            size_t count = std::max(rCount, iCount);
            rStorage.resize(count, 0.0f);
            iStorage.resize(count, 0.0f);
            return makePeriodicWaveValue(rStorage.data(), iStorage.data(),
                                         static_cast<int>(count), disableNorm);
        },
        decoratePeriodicWaveProto);

    // 16. Synth & Sequencer Globals
    installAudioSynthGlobals();
    installAudioSequencerGlobals();

    // 17. getUserMedia: starts engine mic capture and resolves with a
    // MediaStream (for createMediaStreamSource), or rejects with
    // Error("Failed to access microphone"). Installed as the global
    // `__nativeGetUserMedia` and, when the realm already has a `navigator`,
    // as navigator.mediaDevices.getUserMedia — the QuickJS binding's shape.
    ev::setGlobalFunction("__nativeGetUserMedia", 1, [](Value, std::span<const Value>) -> Value {
        ev::Persistent p(ev::createPromise());
        auto* e = getAudioEngine();
        if (!e || !e->startMicCapture()) {
            ev::Persistent err(hostMakeDomError(nullptr, "Failed to access microphone"));
            ev::rejectPromise(p.get(), err.get());
            return p.get();
        }
        ev::Persistent stream(makeMediaStreamValue());
        ev::resolvePromise(p.get(), stream.get());
        return p.get();
    });
    ev::GlobalValue nav = ev::globalValue("navigator");
    if (nav.found && ev::isObject(nav.value)) {
        ev::Persistent navigator(nav.value);
        ev::Persistent mediaDevices(ev::getProperty(navigator.get(), "mediaDevices"));
        if (!ev::isObject(mediaDevices.get())) {
            mediaDevices.set(ev::createObject());
            ev::setProperty(navigator.get(), "mediaDevices", mediaDevices.get());
        }
        ev::Persistent fn(ev::getGlobal("__nativeGetUserMedia"));
        ev::setProperty(mediaDevices.get(), "getUserMedia", fn.get());
    }
}

} // namespace broaudio::api
