#include "host_audio_internal.h"
#include "host_audio_dsp.h"

namespace broaudio::api {

HostClass g_delayNodeClass;
HostClass g_dynamicsCompressorNodeClass;
HostClass g_waveShaperNodeClass;
HostClass g_convolverNodeClass;
HostClass g_channelSplitterNodeClass;
HostClass g_channelMergerNodeClass;

void hostDelayDtor(void* p) {
    delete static_cast<HostDelayNode*>(p);
}

void hostDynamicsCompressorDtor(void* p) {
    delete static_cast<HostDynamicsCompressorNode*>(p);
}

void hostWaveShaperDtor(void* p) {
    delete static_cast<HostWaveShaperNode*>(p);
}

void hostConvolverDtor(void* p) {
    delete static_cast<HostConvolverNode*>(p);
}

void hostChannelSplitterDtor(void* p) {
    delete static_cast<HostChannelSplitterNode*>(p);
}

void hostChannelMergerDtor(void* p) {
    delete static_cast<HostChannelMergerNode*>(p);
}

// ---------------------------------------------------------------------------
// DelayNode
// ---------------------------------------------------------------------------

void decorateDelayNodeProto(ObjectBuilder&) {}

Value makeDelayNodeValue(double maxDelayTime) {
    if (maxDelayTime <= 0.0) maxDelayTime = 1.0;
    if (maxDelayTime > 180.0) maxDelayTime = 180.0;

    auto* delay = new HostDelayNode();
    delay->base.nodeType = AudioNodeType::Delay;
    delay->maxDelayTime = maxDelayTime;

    delay->delayTimeParam = makeAudioParam(AudioParamTarget::DelayTime, -1, 0.0f, 0.0f,
                                           static_cast<float>(maxDelayTime), 0.0f);
    ObjectBuilder b(g_delayNodeClass.make(delay, hostDelayDtor));
    b.set("delayTime", makeAudioParamValue(delay->delayTimeParam));
    return b.get();
}

// ---------------------------------------------------------------------------
// DynamicsCompressorNode
// ---------------------------------------------------------------------------

void processDynamicsCompressor(HostDynamicsCompressorNode* comp,
                               float* buffer,
                               int frames,
                               int channels,
                               int sampleRate,
                               double curTime) {
    if (!comp || !buffer || frames <= 0 || channels <= 0) return;

    float threshold = comp->thresholdParam ? comp->thresholdParam->evaluate(curTime) : -24.0f;
    float knee = comp->kneeParam ? comp->kneeParam->evaluate(curTime) : 30.0f;
    float ratio = comp->ratioParam ? comp->ratioParam->evaluate(curTime) : 12.0f;
    float attack = comp->attackParam ? comp->attackParam->evaluate(curTime) : 0.003f;
    float release = comp->releaseParam ? comp->releaseParam->evaluate(curTime) : 0.25f;

    float sr = static_cast<float>(sampleRate > 0 ? sampleRate : 44100);
    float alphaA = (attack <= 0.0f) ? 1.0f : (1.0f - std::exp(-1.0f / (attack * sr)));
    float alphaR = (release <= 0.0f) ? 1.0f : (1.0f - std::exp(-1.0f / (release * sr)));
    alphaA = std::clamp(alphaA, 0.0f, 1.0f);
    alphaR = std::clamp(alphaR, 0.0f, 1.0f);
    ratio = std::max(1.0f, ratio);
    knee = std::max(0.0f, knee);

    float env = comp->envelope;

    for (int f = 0; f < frames; ++f) {
        float maxAbs = 0.0f;
        for (int c = 0; c < channels; ++c) {
            maxAbs = std::max(maxAbs, std::fabs(buffer[f * channels + c]));
        }

        float xDb = (maxAbs < 1e-5f) ? -100.0f : 20.0f * std::log10(maxAbs);
        float yDb = xDb;

        if (knee > 0.0f && xDb > (threshold - knee * 0.5f) && xDb < (threshold + knee * 0.5f)) {
            float diff = xDb - threshold + knee * 0.5f;
            yDb = xDb + ((1.0f / ratio - 1.0f) * diff * diff) / (2.0f * knee);
        } else if (xDb >= (threshold + knee * 0.5f)) {
            yDb = threshold + (xDb - threshold) / ratio;
        }

        float targetReductionDb = yDb - xDb;
        if (targetReductionDb < env) {
            env += alphaA * (targetReductionDb - env);
        } else {
            env += alphaR * (targetReductionDb - env);
        }

        float gain = std::pow(10.0f, env / 20.0f);
        for (int c = 0; c < channels; ++c) {
            buffer[f * channels + c] *= gain;
        }
    }

    comp->envelope = env;
    comp->reduction = env;
}

void decorateDynamicsCompressorNodeProto(ObjectBuilder& b) {
    b.accessor("reduction", [](Value self_, std::span<const Value>) {
        HostDynamicsCompressorNode* comp = compressorOf(self_);
        return ev::fromDouble(comp ? comp->reduction : 0.0);
    }, nullptr);
}

Value makeDynamicsCompressorNodeValue() {
    auto* comp = new HostDynamicsCompressorNode();
    comp->base.nodeType = AudioNodeType::DynamicsCompressor;

    // The node keeps its own reference to each param's state; the JS
    // AudioParam objects are made and attached one at a time.
    ObjectBuilder b(g_dynamicsCompressorNodeClass.make(comp, hostDynamicsCompressorDtor));
    auto attach = [&b](const char* name, AudioParamTarget target, float init, float lo, float hi) {
        ParamRef p = makeAudioParam(target, -1, init, lo, hi, init);
        b.set(name, makeAudioParamValue(p));
        return p;
    };
    comp->thresholdParam = attach("threshold", AudioParamTarget::CompressorThreshold, -24.0f, -100.0f, 0.0f);
    comp->kneeParam = attach("knee", AudioParamTarget::CompressorKnee, 30.0f, 0.0f, 40.0f);
    comp->ratioParam = attach("ratio", AudioParamTarget::CompressorRatio, 12.0f, 1.0f, 20.0f);
    comp->attackParam = attach("attack", AudioParamTarget::CompressorAttack, 0.003f, 0.0f, 1.0f);
    comp->releaseParam = attach("release", AudioParamTarget::CompressorRelease, 0.25f, 0.0f, 1.0f);
    return b.get();
}

// ---------------------------------------------------------------------------
// WaveShaperNode
// ---------------------------------------------------------------------------

void decorateWaveShaperNodeProto(ObjectBuilder& b) {
    b.accessor("curve",
               [](Value self_, std::span<const Value>) {
                   HostWaveShaperNode* ws = waveShaperOf(self_);
                   if (!ws || ws->curve.empty()) return ev::null();
                   return makeFloat32Array(ws->curve);
               },
               [](Value self_, std::span<const Value> a) {
                   HostWaveShaperNode* ws = waveShaperOf(self_);
                   if (!ws || a.empty()) return ev::undefined();
                   // null / undefined clears the curve.
                   if (ev::isNull(a[0]) || ev::isUndefined(a[0])) {
                       ws->curve.clear();
                       return ev::undefined();
                   }
                   std::vector<float> curve;
                   if (!readFloatArrayArg(a[0], FloatArrayArg::Float32OrPlain, "WaveShaperNode.curve", curve)) {
                       return ev::undefined();
                   }
                   ws->curve = std::move(curve);
                   return ev::undefined();
               });

    b.accessor("oversample",
               [](Value self_, std::span<const Value>) {
                   HostWaveShaperNode* ws = waveShaperOf(self_);
                   return ev::fromUtf8(ws ? ws->oversample : "none");
               },
               [](Value self_, std::span<const Value> a) {
                   HostWaveShaperNode* ws = waveShaperOf(self_);
                   if (!ws || a.empty()) return ev::undefined();
                   std::string o = ev::toUtf8(a[0]);
                   if (o == "none" || o == "2x" || o == "4x") ws->oversample = o;
                   return ev::undefined();
               });
}

Value makeWaveShaperNodeValue() {
    auto* ws = new HostWaveShaperNode();
    ws->base.nodeType = AudioNodeType::WaveShaper;
    return g_waveShaperNodeClass.make(ws, hostWaveShaperDtor);
}

// ---------------------------------------------------------------------------
// ConvolverNode
// ---------------------------------------------------------------------------

void decorateConvolverNodeProto(ObjectBuilder& b) {
    b.accessor("buffer",
               [](Value self_, std::span<const Value>) {
                   Value v = ev::getProperty(self_, "_buffer");
                   return ev::isUndefined(v) ? ev::null() : v;
               },
               [](Value self_, std::span<const Value> a) {
                   HostConvolverNode* conv = convolverOf(self_);
                   if (!conv) return ev::undefined();
                   const bool clear = a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0]);
                   if (!clear && !hostAudioBufferOf(a[0])) {
                       return ev::throwTypeError("ConvolverNode.buffer must be an AudioBuffer or null");
                   }
                   // Unwrap before the write: setProperty moves the heap.
                   conv->buffer = clear ? nullptr : hostAudioBufferRef(a[0]);
                   ev::setProperty(self_, "_buffer", clear ? ev::null() : a[0]);
                   return ev::undefined();
               });

    b.accessor("normalize",
               [](Value self_, std::span<const Value>) {
                   HostConvolverNode* conv = convolverOf(self_);
                   return ev::fromBool(conv ? conv->normalize : true);
               },
               [](Value self_, std::span<const Value> a) {
                   HostConvolverNode* conv = convolverOf(self_);
                   if (conv) conv->normalize = boolAt(a, 0);
                   return ev::undefined();
               });
}

Value makeConvolverNodeValue() {
    auto* conv = new HostConvolverNode();
    conv->base.nodeType = AudioNodeType::Convolver;
    return g_convolverNodeClass.make(conv, hostConvolverDtor);
}

// ---------------------------------------------------------------------------
// ChannelSplitterNode & ChannelMergerNode
// ---------------------------------------------------------------------------

void decorateChannelSplitterNodeProto(ObjectBuilder& b) {
    b.accessor("numberOfOutputs", [](Value self_, std::span<const Value>) {
        HostChannelSplitterNode* s = channelSplitterOf(self_);
        return ev::fromDouble(s ? s->numberOfOutputs : 6);
    }, nullptr);
    b.accessor("channelCount", [](Value self_, std::span<const Value>) {
        HostChannelSplitterNode* s = channelSplitterOf(self_);
        return ev::fromDouble(s ? s->numberOfOutputs : 6);
    }, [](Value, std::span<const Value>) {
        return ev::undefined();
    });
    b.set("channelCountMode", ev::fromUtf8("explicit"));
    b.set("channelInterpretation", ev::fromUtf8("discrete"));
}

Value makeChannelSplitterNodeValue(int numberOfOutputs) {
    if (numberOfOutputs <= 0) numberOfOutputs = 6;
    if (numberOfOutputs > 32) numberOfOutputs = 32;

    auto* s = new HostChannelSplitterNode();
    s->base.nodeType = AudioNodeType::ChannelSplitter;
    s->numberOfOutputs = numberOfOutputs;
    return g_channelSplitterNodeClass.make(s, hostChannelSplitterDtor);
}

void decorateChannelMergerNodeProto(ObjectBuilder& b) {
    b.accessor("numberOfInputs", [](Value self_, std::span<const Value>) {
        HostChannelMergerNode* m = channelMergerOf(self_);
        return ev::fromDouble(m ? m->numberOfInputs : 6);
    }, nullptr);
    b.accessor("channelCount", [](Value, std::span<const Value>) {
        return ev::fromDouble(1.0);
    }, [](Value, std::span<const Value>) {
        return ev::undefined();
    });
    b.set("channelCountMode", ev::fromUtf8("explicit"));
    b.set("channelInterpretation", ev::fromUtf8("speakers"));
}

Value makeChannelMergerNodeValue(int numberOfInputs) {
    if (numberOfInputs <= 0) numberOfInputs = 6;
    if (numberOfInputs > 32) numberOfInputs = 32;

    auto* m = new HostChannelMergerNode();
    m->base.nodeType = AudioNodeType::ChannelMerger;
    m->numberOfInputs = numberOfInputs;
    return g_channelMergerNodeClass.make(m, hostChannelMergerDtor);
}

// ---------------------------------------------------------------------------
// AudioContext Bus Methods
// ---------------------------------------------------------------------------

void registerAudioContextBuses(ObjectBuilder& b) {
    b.def("createBus", 0, [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e ? e->createBus() : -1);
    });

    b.def("deleteBus", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->deleteBus(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setBusGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusGain", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusGain(i32At(a, 0)) : 1.0);
    });

    b.def("setBusPan", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusPan(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusPan", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusPan(i32At(a, 0)) : 0.0);
    });

    b.def("setBusMuted", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusMuted(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusMuted", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->getBusMuted(i32At(a, 0)) : false);
    });

    b.def("setBusSolo", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusSolo(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusSolo", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->getBusSolo(i32At(a, 0)) : false);
    });

    b.def("setBusSend", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setBusSend(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusPeakL", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusPeakL(i32At(a, 0)) : 0.0);
    });

    b.def("getBusPeakR", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusPeakR(i32At(a, 0)) : 0.0);
    });

    b.def("getBusRmsL", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusRmsL(i32At(a, 0)) : 0.0);
    });

    b.def("getBusRmsR", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusRmsR(i32At(a, 0)) : 0.0);
    });

    // Filters
    b.def("allocateBusFilterSlot", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->allocateBusFilterSlot(i32At(a, 0)) : -1);
    });

    b.def("releaseBusFilterSlot", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->releaseBusFilterSlot(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("setBusFilterEnabled", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setBusFilterEnabled(i32At(a, 0), i32At(a, 1), boolAt(a, 2));
        return ev::undefined();
    });

    b.def("getBusFilterEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && a.size() >= 2 ? e->getBusFilterEnabled(i32At(a, 0), i32At(a, 1)) : false);
    });

    b.def("setBusFilterType", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setBusFilterType(i32At(a, 0), i32At(a, 1), parseFilterType(ev::toUtf8(a[2])));
        return ev::undefined();
    });

    b.def("getBusFilterType", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromUtf8(e && a.size() >= 2 ? filterTypeToString(e->getBusFilterType(i32At(a, 0), i32At(a, 1))) : "lowpass");
    });

    b.def("setBusFilterFrequency", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setBusFilterFrequency(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusFilterFrequency", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && a.size() >= 2 ? e->getBusFilterFrequency(i32At(a, 0), i32At(a, 1)) : 1000.0);
    });

    b.def("setBusFilterQ", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setBusFilterQ(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusFilterQ", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && a.size() >= 2 ? e->getBusFilterQ(i32At(a, 0), i32At(a, 1)) : 1.0);
    });

    b.def("setBusFilterGain", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) e->setBusFilterGain(i32At(a, 0), i32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("getBusFilterGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && a.size() >= 2 ? e->getBusFilterGain(i32At(a, 0), i32At(a, 1)) : 0.0);
    });

    // Delay
    b.def("setBusDelayEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDelayEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusDelayEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->getBusDelayEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusDelayTime", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDelayTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDelayTime", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusDelayTime(i32At(a, 0)) : 0.0);
    });

    b.def("setBusDelayFeedback", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDelayFeedback(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDelayFeedback", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusDelayFeedback(i32At(a, 0)) : 0.0);
    });

    b.def("setBusDelayMix", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusDelayMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusDelayMix", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusDelayMix(i32At(a, 0)) : 0.0);
    });

    // Reverb
    b.def("setBusReverbEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusReverbEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusReverbEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->getBusReverbEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusReverbRoomSize", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusReverbRoomSize(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusReverbRoomSize", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusReverbRoomSize(i32At(a, 0)) : 0.0);
    });

    b.def("setBusReverbDamping", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusReverbDamping(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusReverbDamping", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusReverbDamping(i32At(a, 0)) : 0.0);
    });

    b.def("setBusReverbMix", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusReverbMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusReverbMix", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusReverbMix(i32At(a, 0)) : 0.0);
    });

    // Chorus
    b.def("setBusChorusEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusChorusEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusChorusEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->getBusChorusEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusChorusRate", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusChorusRate(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusRate", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusRate(i32At(a, 0)) : 1.0);
    });

    b.def("setBusChorusDepth", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusChorusDepth(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusDepth", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusDepth(i32At(a, 0)) : 0.0);
    });

    b.def("setBusChorusMix", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusChorusMix(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusMix", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusMix(i32At(a, 0)) : 0.0);
    });

    b.def("setBusChorusBaseDelay", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusChorusBaseDelay(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusChorusBaseDelay", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusChorusBaseDelay(i32At(a, 0)) : 0.0);
    });

    // Compressor
    b.def("setBusCompressorEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusCompressorEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("getBusCompressorEnabled", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromBool(e && !a.empty() ? e->getBusCompressorEnabled(i32At(a, 0)) : false);
    });

    b.def("setBusCompressorThreshold", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusCompressorThreshold(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusCompressorThreshold", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorThreshold(i32At(a, 0)) : 0.0);
    });

    b.def("setBusCompressorRatio", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusCompressorRatio(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusCompressorRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorRatio(i32At(a, 0)) : 1.0);
    });

    b.def("setBusCompressorAttack", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusCompressorAttack(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusCompressorAttack", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorAttack(i32At(a, 0)) : 0.0);
    });

    b.def("setBusCompressorRelease", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusCompressorRelease(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getBusCompressorRelease", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorRelease(i32At(a, 0)) : 0.0);
    });

    b.def("setBusCompressorSidechain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setBusCompressorSidechain(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("getBusCompressorSidechain", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e && !a.empty() ? e->getBusCompressorSidechain(i32At(a, 0)) : -1);
    });
}

} // namespace broaudio::api
