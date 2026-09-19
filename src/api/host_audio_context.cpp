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
    auto* p = static_cast<HostAudioContext*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioContextTag) return nullptr;
    return p;
}

Value makeAudioContextValue() {
    auto* ctx = new HostAudioContext();
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

    b.accessor("state", [](Value, std::span<const Value>) {
        return ev::fromUtf8("running");
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
    b.def("suspend", 0, [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        if (e) e->setMasterPaused(true);
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("resume", 0, [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        if (e) e->setMasterPaused(false);
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("close", 0, [](Value, std::span<const Value>) {
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    // 3. Node factories
    b.def("createGain", 0, [](Value, std::span<const Value>) {
        return makeGainNodeValue();
    });

    b.def("createOscillator", 0, [](Value, std::span<const Value>) {
        return makeOscillatorNodeValue();
    });

    b.def("createPeriodicWave", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::null();
        const uint8_t* rData = nullptr; size_t rLen = 0, rElem = 1;
        const uint8_t* iData = nullptr; size_t iLen = 0, iElem = 1;
        if (!bufferBytes(a[0], &rData, &rLen, &rElem) || !bufferBytes(a[1], &iData, &iLen, &iElem)) {
            return ev::null();
        }
        int count = static_cast<int>(std::min(rLen, iLen) / sizeof(float));
        bool disableNorm = false;
        if (a.size() >= 3 && ev::isObject(a[2])) {
            Value opt = a[2];
            Value dn = ev::getProperty(opt, "disableNormalization");
            if (ev::isBool(dn)) disableNorm = ev::toBool(dn);
        }
        return makePeriodicWaveValue(reinterpret_cast<const float*>(rData),
                                     reinterpret_cast<const float*>(iData),
                                     count, disableNorm);
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
        int sr = a.size() >= 3 ? i32At(a, 2) : 44100;
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
        return makeSequenceValue(va);
    });

    // 4. decodeAudioData
    b.def("decodeAudioData", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        Value inputV = a[0];
        Value successCb = a.size() >= 2 ? a[1] : ev::undefined();
        Value errorCb = a.size() >= 3 ? a[2] : ev::undefined();

        const uint8_t* rawData = nullptr;
        size_t rawLen = 0, elemSize = 1;
        if (!bufferBytes(inputV, &rawData, &rawLen, &elemSize) || rawLen == 0) {
            if (ev::isFunction(errorCb)) {
                Value err = hostMakeDomError("EncodingError", "decodeAudioData: invalid buffer");
                ev::call(errorCb, ev::undefined(), std::span<const Value>(&err, 1));
            }
            return ev::null();
        }

        broaudio::AudioFileData data = broaudio::loadAudioFileFromMemory(rawData, rawLen);
        if (!data.valid()) {
            if (ev::isFunction(errorCb)) {
                std::string msg = data.error.empty() ? "decodeAudioData: failed to decode audio" : data.error;
                Value err = hostMakeDomError("EncodingError", msg);
                ev::call(errorCb, ev::undefined(), std::span<const Value>(&err, 1));
            }
            return ev::null();
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

        ObjectBuilder res;
        Value samplesArr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(samples.size()));
        ev::fillTypedArray(samplesArr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(samples.data()),
                                                                samples.size() * sizeof(float)));
        res.set("samples", samplesArr);
        res.set("channels", ev::fromDouble(data.channels));
        res.set("sampleRate", ev::fromDouble(engRate));
        res.set("numFrames", ev::fromDouble(numFrames));
        res.set("numberOfChannels", ev::fromDouble(data.channels));
        res.set("length", ev::fromDouble(numFrames));
        res.set("duration", ev::fromDouble(static_cast<double>(numFrames) / engRate));

        int chCount = data.channels;
        res.def("getChannelData", 1, [samples, chCount, numFrames](Value, std::span<const Value> ca) -> Value {
            int c = ca.empty() ? 0 : i32At(ca, 0);
            if (c < 0 || c >= chCount || numFrames <= 0) return ev::null();
            std::vector<float> ch(numFrames);
            for (int i = 0; i < numFrames; ++i) ch[i] = samples[i * chCount + c];
            Value out = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(numFrames));
            ev::fillTypedArray(out, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(ch.data()),
                                                            numFrames * sizeof(float)));
            return out;
        });

        res.def("then", 2, [](Value self, std::span<const Value> ta) -> Value {
            if (!ta.empty() && ev::isFunction(ta[0])) {
                try {
                    ev::call(ta[0], ev::undefined(), std::span<const Value>(&self, 1));
                } catch (...) {}
            }
            return self;
        });

        res.def("catch", 1, [](Value self, std::span<const Value>) -> Value {
            return self;
        });

        Value resVal = res.get();
        if (ev::isFunction(successCb)) {
            try {
                ev::call(successCb, ev::undefined(), std::span<const Value>(&resVal, 1));
            } catch (...) {}
        }
        return resVal;
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
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[1]);
        if (!info || !info.data) return ev::throwTypeError("Expected Float32Array as second argument");
        int channels = i32At(a, 2);
        int sampleRate = i32At(a, 3);
        if (channels <= 0 || sampleRate <= 0) return ev::fromBool(false);
        int frames = static_cast<int>(info.byteLength / sizeof(float)) / channels;
        return ev::fromBool(broaudio::saveWav(path.c_str(), reinterpret_cast<const float*>(info.data),
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
            int length = 0, channels = 1, sampleRate = 44100;
            if (!a.empty() && ev::isObject(a[0])) {
                Value opt = a[0];
                Value lenV = ev::getProperty(opt, "length");
                if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) length = static_cast<int>(ev::toDouble(lenV));
                Value chV = ev::getProperty(opt, "numberOfChannels");
                if (!ev::isUndefined(chV) && !ev::isObject(chV)) channels = static_cast<int>(ev::toDouble(chV));
                Value srV = ev::getProperty(opt, "sampleRate");
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
            std::vector<float> rStorage, iStorage;
            const float* rData = nullptr;
            const float* iData = nullptr;
            size_t rCount = 0, iCount = 0;
            if (!a.empty()) floatData(a[0], rStorage, &rData, &rCount);
            if (a.size() >= 2) floatData(a[1], iStorage, &iData, &iCount);
            bool disableNorm = false;
            if (a.size() >= 3 && ev::isObject(a[2])) {
                Value opt = a[2];
                Value dn = ev::getProperty(opt, "disableNormalization");
                if (!ev::isUndefined(dn)) disableNorm = ev::toBool(dn);
            }
            int count = static_cast<int>(std::max(rCount, iCount));
            return makePeriodicWaveValue(rData, iData, count, disableNorm);
        },
        decoratePeriodicWaveProto);

    // 16. Synth & Sequencer Globals
    installAudioSynthGlobals();
    installAudioSequencerGlobals();
}

} // namespace broaudio::api
