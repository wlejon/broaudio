#include "host_audio_internal.h"

namespace broaudio::api {

HostClass g_audioNodeClass;
HostClass g_audioDestinationNodeClass;
HostClass g_gainNodeClass;
HostClass g_oscillatorNodeClass;
HostClass g_periodicWaveClass;
HostClass g_biquadFilterNodeClass;
HostClass g_analyserNodeClass;

broaudio::BiquadFilter::Type parseFilterType(const std::string& str) {
    if (str == "highpass") return broaudio::BiquadFilter::Type::Highpass;
    if (str == "bandpass") return broaudio::BiquadFilter::Type::Bandpass;
    if (str == "notch") return broaudio::BiquadFilter::Type::Notch;
    if (str == "allpass") return broaudio::BiquadFilter::Type::Allpass;
    if (str == "peaking") return broaudio::BiquadFilter::Type::Peaking;
    if (str == "lowshelf") return broaudio::BiquadFilter::Type::Lowshelf;
    if (str == "highshelf") return broaudio::BiquadFilter::Type::Highshelf;
    return broaudio::BiquadFilter::Type::Lowpass;
}

const char* filterTypeToString(broaudio::BiquadFilter::Type type) {
    switch (type) {
        case broaudio::BiquadFilter::Type::Highpass: return "highpass";
        case broaudio::BiquadFilter::Type::Bandpass: return "bandpass";
        case broaudio::BiquadFilter::Type::Notch: return "notch";
        case broaudio::BiquadFilter::Type::Allpass: return "allpass";
        case broaudio::BiquadFilter::Type::Peaking: return "peaking";
        case broaudio::BiquadFilter::Type::Lowshelf: return "lowshelf";
        case broaudio::BiquadFilter::Type::Highshelf: return "highshelf";
        case broaudio::BiquadFilter::Type::Lowpass:
        default:
            return "lowpass";
    }
}

broaudio::Waveform parseWaveform(const std::string& str) {
    if (str == "square") return broaudio::Waveform::Square;
    if (str == "sawtooth") return broaudio::Waveform::Sawtooth;
    if (str == "triangle") return broaudio::Waveform::Triangle;
    if (str == "wavetable" || str == "custom") return broaudio::Waveform::Wavetable;
    if (str == "whitenoise") return broaudio::Waveform::WhiteNoise;
    if (str == "pinknoise") return broaudio::Waveform::PinkNoise;
    if (str == "brownnoise") return broaudio::Waveform::BrownNoise;
    return broaudio::Waveform::Sine;
}

const char* waveformToString(broaudio::Waveform wf) {
    switch (wf) {
        case broaudio::Waveform::Square: return "square";
        case broaudio::Waveform::Sawtooth: return "sawtooth";
        case broaudio::Waveform::Triangle: return "triangle";
        case broaudio::Waveform::Wavetable: return "custom";
        case broaudio::Waveform::WhiteNoise: return "whitenoise";
        case broaudio::Waveform::PinkNoise: return "pinknoise";
        case broaudio::Waveform::BrownNoise: return "brownnoise";
        case broaudio::Waveform::Sine:
        default:
            return "sine";
    }
}

void hostAudioNodeDtor(void* p) {
    delete static_cast<HostAudioNode*>(p);
}

void hostGainDtor(void* p) {
    delete static_cast<HostGainNode*>(p);
}

void hostOscillatorDtor(void* p) {
    auto* osc = static_cast<HostOscillatorNode*>(p);
    if (osc) {
        if (osc->voiceId >= 0) {
            auto* e = getAudioEngine();
            if (e) {
                e->stopVoice(osc->voiceId, e->currentTime());
                e->removeVoice(osc->voiceId);
            }
        }
        delete osc;
    }
}

void hostPeriodicWaveDtor(void* p) {
    delete static_cast<HostPeriodicWave*>(p);
}

void hostBiquadFilterDtor(void* p) {
    auto* filter = static_cast<HostBiquadFilterNode*>(p);
    if (filter) {
        if (filter->slot >= 0) {
            auto* e = getAudioEngine();
            if (e) {
                e->releaseFilterSlot(filter->slot);
            }
        }
        delete filter;
    }
}

void hostAnalyserDtor(void* p) {
    delete static_cast<HostAnalyserNode*>(p);
}

HostAudioNode* hostAudioNodeOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioNode*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioNodeTag) return nullptr;
    return p;
}

HostPeriodicWave* hostPeriodicWaveOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostPeriodicWave*>(ev::handleData(v));
    if (!p || p->tag != kHostPeriodicWaveTag) return nullptr;
    return p;
}

// connect/disconnect live once on AudioNode.prototype (the class check pins
// that); broaudio tracks connectedTargets on every AudioNode for graph
// traversal (e.g. GainNode volume and PannerNode spatialization), and dispatches
// on the node kind for connections that mean something to the engine:
// oscillator -> gain remembers the GainNode so start() reads gain.value,
// biquad connect/disconnect enables/disables its filter slot, and the mic
// source connecting to an analyser points the analyser at the mic ring.
void decorateAudioNodeProto(ObjectBuilder& b) {
    b.def("connect", 3, [](Value self_, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("AudioNode.connect: destination argument required");
        HostAudioNode* node = hostAudioNodeOf(self_);
        if (node) {
            node->connectedTargets.emplace_back(a[0]);
            switch (node->nodeType) {
            case AudioNodeType::Oscillator:
                if (nodeOfKind<HostGainNode>(a[0], AudioNodeType::Gain)) {
                    ev::setProperty(self_, "_connectedGain", a[0]);
                }
                break;
            case AudioNodeType::BiquadFilter:
                if (HostBiquadFilterNode* filter = filterOf(self_)) {
                    auto* eng = getAudioEngine();
                    if (eng && filter->slot >= 0) eng->setFilterEnabled(filter->slot, true);
                }
                break;
            case AudioNodeType::Delay:
                if (HostDelayNode* delay = delayOf(self_)) {
                    auto* eng = getAudioEngine();
                    if (eng) {
                        eng->setDelayEnabled(true);
                        float dt = delay->delayTimeParam ? delay->delayTimeParam->value : 0.0f;
                        eng->setDelayTime(dt);
                        eng->setDelayMix(1.0f);
                    }
                }
                break;
            case AudioNodeType::DynamicsCompressor:
                if (HostDynamicsCompressorNode* comp = compressorOf(self_)) {
                    auto* eng = getAudioEngine();
                    if (eng) {
                        eng->setBusCompressorEnabled(Engine::MASTER_BUS_ID, true);
                        float th = comp->thresholdParam ? comp->thresholdParam->value : -24.0f;
                        float ra = comp->ratioParam ? comp->ratioParam->value : 12.0f;
                        float at = comp->attackParam ? comp->attackParam->value * 1000.0f : 3.0f;
                        float re = comp->releaseParam ? comp->releaseParam->value * 1000.0f : 250.0f;
                        eng->setBusCompressorThreshold(Engine::MASTER_BUS_ID, th);
                        eng->setBusCompressorRatio(Engine::MASTER_BUS_ID, ra);
                        eng->setBusCompressorAttack(Engine::MASTER_BUS_ID, at);
                        eng->setBusCompressorRelease(Engine::MASTER_BUS_ID, re);
                    }
                }
                break;
            case AudioNodeType::WaveShaper:
                if (HostWaveShaperNode* ws = waveShaperOf(self_)) {
                    auto* eng = getAudioEngine();
                    if (eng) {
                        eng->setBusDistortionEnabled(Engine::MASTER_BUS_ID, true);
                        eng->setBusDistortionMode(Engine::MASTER_BUS_ID, DistortionMode::SoftClip);
                        eng->setBusDistortionMix(Engine::MASTER_BUS_ID, 1.0f);
                    }
                }
                break;
            case AudioNodeType::Convolver:
                if (HostConvolverNode* conv = convolverOf(self_)) {
                    auto* eng = getAudioEngine();
                    if (eng) {
                        eng->setBusReverbEnabled(Engine::MASTER_BUS_ID, true);
                        eng->setBusReverbMix(Engine::MASTER_BUS_ID, 1.0f);
                    }
                }
                break;
            case AudioNodeType::MediaStreamSource:
                if (HostAnalyserNode* analyser = analyserOf(a[0])) analyser->source = 1;
                break;
            default:
                if (HostAnalyserNode* analyser = analyserOf(a[0])) analyser->hasConnectedInput = true;
                break;
            }

            HostAudioNode* destNode = hostAudioNodeOf(a[0]);
            if (destNode) {
                auto* eng = getAudioEngine();
                if (eng) {
                    if (destNode->nodeType == AudioNodeType::Delay) {
                        HostDelayNode* delay = reinterpret_cast<HostDelayNode*>(destNode);
                        eng->setDelayEnabled(true);
                        float dt = delay && delay->delayTimeParam ? delay->delayTimeParam->value : 0.0f;
                        eng->setDelayTime(dt);
                        eng->setDelayMix(1.0f);
                    } else if (destNode->nodeType == AudioNodeType::DynamicsCompressor) {
                        HostDynamicsCompressorNode* comp = reinterpret_cast<HostDynamicsCompressorNode*>(destNode);
                        eng->setBusCompressorEnabled(Engine::MASTER_BUS_ID, true);
                        if (comp) {
                            float th = comp->thresholdParam ? comp->thresholdParam->value : -24.0f;
                            float ra = comp->ratioParam ? comp->ratioParam->value : 12.0f;
                            float at = comp->attackParam ? comp->attackParam->value * 1000.0f : 3.0f;
                            float re = comp->releaseParam ? comp->releaseParam->value * 1000.0f : 250.0f;
                            eng->setBusCompressorThreshold(Engine::MASTER_BUS_ID, th);
                            eng->setBusCompressorRatio(Engine::MASTER_BUS_ID, ra);
                            eng->setBusCompressorAttack(Engine::MASTER_BUS_ID, at);
                            eng->setBusCompressorRelease(Engine::MASTER_BUS_ID, re);
                        }
                    } else if (destNode->nodeType == AudioNodeType::WaveShaper) {
                        eng->setBusDistortionEnabled(Engine::MASTER_BUS_ID, true);
                        eng->setBusDistortionMode(Engine::MASTER_BUS_ID, DistortionMode::SoftClip);
                        eng->setBusDistortionMix(Engine::MASTER_BUS_ID, 1.0f);
                    } else if (destNode->nodeType == AudioNodeType::Convolver) {
                        eng->setBusReverbEnabled(Engine::MASTER_BUS_ID, true);
                        eng->setBusReverbMix(Engine::MASTER_BUS_ID, 1.0f);
                    }
                }
            }
        }
        return a[0];
    });

    b.def("disconnect", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAudioNode* node = hostAudioNodeOf(self_);
        if (node) {
            if (a.empty() || ev::isUndefined(a[0])) {
                for (auto& t : node->connectedTargets) {
                    if (HostAnalyserNode* an = analyserOf(t.get())) an->hasConnectedInput = false;
                    t.set(ev::undefined());
                }
                node->connectedTargets.clear();
            } else {
                for (auto it = node->connectedTargets.begin(); it != node->connectedTargets.end();) {
                    if (it->get() == a[0] || ev::handleData(it->get()) == ev::handleData(a[0])) {
                        if (HostAnalyserNode* an = analyserOf(it->get())) an->hasConnectedInput = false;
                        it->set(ev::undefined());
                        it = node->connectedTargets.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (node->nodeType == AudioNodeType::Oscillator) {
                ev::setProperty(self_, "_connectedGain", ev::undefined());
            } else if (node->nodeType == AudioNodeType::BiquadFilter) {
                if (HostBiquadFilterNode* filter = filterOf(self_)) {
                    auto* eng = getAudioEngine();
                    if (eng && filter->slot >= 0) eng->setFilterEnabled(filter->slot, false);
                }
            } else if (node->nodeType == AudioNodeType::Delay) {
                auto* eng = getAudioEngine();
                if (eng) eng->setDelayEnabled(false);
            } else if (node->nodeType == AudioNodeType::DynamicsCompressor) {
                auto* eng = getAudioEngine();
                if (eng) eng->setBusCompressorEnabled(Engine::MASTER_BUS_ID, false);
            } else if (node->nodeType == AudioNodeType::WaveShaper) {
                auto* eng = getAudioEngine();
                if (eng) eng->setBusDistortionEnabled(Engine::MASTER_BUS_ID, false);
            } else if (node->nodeType == AudioNodeType::Convolver) {
                auto* eng = getAudioEngine();
                if (eng) eng->setBusReverbEnabled(Engine::MASTER_BUS_ID, false);
            }
        }
        return ev::undefined();
    });

    b.accessor("numberOfInputs", [](Value self_, std::span<const Value>) {
        HostAudioNode* node = hostAudioNodeOf(self_);
        if (!node) return ev::fromDouble(1.0);
        if (node->nodeType == AudioNodeType::Oscillator ||
            node->nodeType == AudioNodeType::BufferSource) {
            return ev::fromDouble(0.0);
        }
        if (node->nodeType == AudioNodeType::ChannelMerger) {
            auto* m = reinterpret_cast<HostChannelMergerNode*>(node);
            return ev::fromDouble(m->numberOfInputs);
        }
        return ev::fromDouble(1.0);
    }, nullptr);

    b.accessor("numberOfOutputs", [](Value self_, std::span<const Value>) {
        HostAudioNode* node = hostAudioNodeOf(self_);
        if (!node) return ev::fromDouble(1.0);
        if (node->nodeType == AudioNodeType::Destination) {
            return ev::fromDouble(0.0);
        }
        if (node->nodeType == AudioNodeType::ChannelSplitter) {
            auto* s = reinterpret_cast<HostChannelSplitterNode*>(node);
            return ev::fromDouble(s->numberOfOutputs);
        }
        return ev::fromDouble(1.0);
    }, nullptr);

    b.accessor("channelCount", [](Value, std::span<const Value>) {
        return ev::fromDouble(2.0);
    }, [](Value, std::span<const Value>) {
        return ev::undefined();
    });

    b.set("channelCountMode", ev::fromUtf8("max"));
    b.set("channelInterpretation", ev::fromUtf8("speakers"));
}

Value makeGainNodeValue() {
    auto* gain = new HostGainNode();
    gain->base.nodeType = AudioNodeType::Gain;
    // The param is rooted across the node's own allocation.
    ev::Persistent gainParam(makeAudioParamValue(AudioParamTarget::Gain, -1, 1.0f, -3.4e38f, 3.4e38f, 1.0f));
    gain->gainParam = hostAudioParamOf(gainParam.get());
    ObjectBuilder b(g_gainNodeClass.make(gain, hostGainDtor));
    b.set("gain", gainParam.get());
    return b.get();
}

void decoratePeriodicWaveProto(ObjectBuilder&) {}

Value makePeriodicWaveValue(const float* real, const float* imag, int count, bool disableNorm) {
    auto* pw = new HostPeriodicWave();
    pw->disableNormalization = disableNorm;
    if (real && count > 0) pw->real.assign(real, real + count);
    if (imag && count > 0) pw->imag.assign(imag, imag + count);

    if (count > 0) {
        constexpr int N = broaudio::WavetableBank::TABLE_SIZE;
        std::vector<float> table(N, 0.0f);
        for (int n = 0; n < N; ++n) {
            double phase = 2.0 * M_PI * n / N;
            double sum = 0.0;
            for (int k = 0; k < count; ++k) {
                double r = (k < static_cast<int>(pw->real.size())) ? pw->real[k] : 0.0;
                double im = (k < static_cast<int>(pw->imag.size())) ? pw->imag[k] : 0.0;
                sum += r * std::cos(k * phase) + im * std::sin(k * phase);
            }
            table[n] = static_cast<float>(sum);
        }

        if (!disableNorm) {
            float maxAbs = 0.0f;
            for (float s : table) maxAbs = std::max(maxAbs, std::abs(s));
            if (maxAbs > 1e-6f) {
                float inv = 1.0f / maxAbs;
                for (float& s : table) s *= inv;
            }
        }

        auto* eng = getAudioEngine();
        int sr = eng ? eng->sampleRate() : 44100;
        pw->wavetable = broaudio::WavetableBank::createFromWaveform(table.data(), N, sr);
    }

    return g_periodicWaveClass.make(pw, hostPeriodicWaveDtor);
}

void decorateOscillatorNodeProto(ObjectBuilder& b) {
    // connect(gain) is handled by the shared AudioNode.connect (see
    // decorateAudioNodeProto); start() reads the remembered gain.value.
    b.accessor("type",
               [](Value self_, std::span<const Value>) {
                   HostOscillatorNode* osc = oscOf(self_);
                   if (!osc) return ev::undefined();
                   return ev::fromUtf8(osc->type);
               },
               [](Value self_, std::span<const Value> a) {
                   HostOscillatorNode* osc = oscOf(self_);
                   if (!osc || a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string t = ev::toUtf8(a[0]);
                   osc->type = t;
                   auto* eng = getAudioEngine();
                   if (eng && osc->voiceId >= 0) {
                       eng->setWaveform(osc->voiceId, parseWaveform(t));
                   }
                   return ev::undefined();
               });

    // start(when = now): applies the connected GainNode's gain.value to the
    // voice first, so `osc.connect(gain); gain.gain.value = 0.3; osc.start()`
    // plays at 0.3 the way the graph says.
    b.def("start", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostOscillatorNode* osc = oscOf(self_);
        if (!osc) return ev::undefined();
        if (osc->started) return ev::throwError("OscillatorNode cannot be started more than once");
        osc->started = true;
        auto* eng = getAudioEngine();
        if (!eng || osc->voiceId < 0) return ev::undefined();
        double when = hasArg(a, 0) ? numAt(a, 0) : eng->currentTime();
        Value connGain = ev::getProperty(self_, "_connectedGain");
        if (ev::isObject(connGain)) {
            Value gainParam = ev::getProperty(connGain, "gain");
            if (ev::isObject(gainParam)) {
                Value v = ev::getProperty(gainParam, "value");
                if (ev::isNumber(v)) eng->setGain(osc->voiceId, static_cast<float>(ev::toDouble(v)));
            }
            // `osc` is host memory and does not move; self_ is stale from
            // here on and is not read again.
        }

        std::vector<HostAudioNode*> queue;
        std::vector<HostAudioNode*> visited;
        pushConnectedTargets(osc->base.connectedTargets, queue, when);
        while (!queue.empty()) {
            HostAudioNode* cur = queue.back();
            queue.pop_back();
            if (std::find(visited.begin(), visited.end(), cur) != visited.end()) continue;
            visited.push_back(cur);

            if (cur->nodeType == AudioNodeType::Gain) {
                auto* gn = reinterpret_cast<HostGainNode*>(cur);
                if (gn->gainParam) {
                    eng->setGain(osc->voiceId, gn->gainParam->evaluate(when));
                }
            } else if (cur->nodeType == AudioNodeType::StereoPanner) {
                auto* sp = reinterpret_cast<HostStereoPannerNode*>(cur);
                float panVal = sp->panParam ? sp->panParam->evaluate(when) : sp->pan;
                eng->setVoicePan(osc->voiceId, panVal);
            } else if (cur->nodeType == AudioNodeType::Panner) {
                auto* pn = reinterpret_cast<HostPannerNode*>(cur);
                float px = pn->posX;
                float py = pn->posY;
                float pz = pn->posZ;
                eng->setVoiceSpatialEnabled(osc->voiceId, true);
                eng->setVoiceSpatialPosition(osc->voiceId, px, py, pz);
                eng->setVoiceSpatialRefDistance(osc->voiceId, pn->refDistance);
                eng->setVoiceSpatialMaxDistance(osc->voiceId, pn->maxDistance);
                eng->setVoiceSpatialRolloff(osc->voiceId, pn->rolloffFactor);
                eng->setVoiceSpatialDistanceModel(osc->voiceId, parseDistanceModel(pn->distanceModel));
            } else if (cur->nodeType == AudioNodeType::Delay) {
                auto* dn = reinterpret_cast<HostDelayNode*>(cur);
                eng->setDelayEnabled(true);
                float dt = dn->delayTimeParam ? dn->delayTimeParam->evaluate(when) : 0.0f;
                eng->setDelayTime(dt);
                eng->setDelayMix(1.0f);
            } else if (cur->nodeType == AudioNodeType::DynamicsCompressor) {
                auto* comp = reinterpret_cast<HostDynamicsCompressorNode*>(cur);
                eng->setBusCompressorEnabled(Engine::MASTER_BUS_ID, true);
                float th = comp->thresholdParam ? comp->thresholdParam->evaluate(when) : -24.0f;
                float ra = comp->ratioParam ? comp->ratioParam->evaluate(when) : 12.0f;
                float at = comp->attackParam ? comp->attackParam->evaluate(when) * 1000.0f : 3.0f;
                float re = comp->releaseParam ? comp->releaseParam->evaluate(when) * 1000.0f : 250.0f;
                eng->setBusCompressorThreshold(Engine::MASTER_BUS_ID, th);
                eng->setBusCompressorRatio(Engine::MASTER_BUS_ID, ra);
                eng->setBusCompressorAttack(Engine::MASTER_BUS_ID, at);
                eng->setBusCompressorRelease(Engine::MASTER_BUS_ID, re);
            } else if (cur->nodeType == AudioNodeType::WaveShaper) {
                eng->setBusDistortionEnabled(Engine::MASTER_BUS_ID, true);
                eng->setBusDistortionMode(Engine::MASTER_BUS_ID, DistortionMode::SoftClip);
                eng->setBusDistortionMix(Engine::MASTER_BUS_ID, 1.0f);
            } else if (cur->nodeType == AudioNodeType::Convolver) {
                eng->setBusReverbEnabled(Engine::MASTER_BUS_ID, true);
                eng->setBusReverbMix(Engine::MASTER_BUS_ID, 1.0f);
            }

            pushConnectedTargets(cur->connectedTargets, queue, when);
        }

        eng->startVoice(osc->voiceId, when);
        return ev::undefined();
    });

    b.def("stop", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostOscillatorNode* osc = oscOf(self_);
        if (!osc) return ev::undefined();
        osc->stopped = true;
        auto* eng = getAudioEngine();
        if (eng && osc->voiceId >= 0) {
            double when = hasArg(a, 0) ? numAt(a, 0) : eng->currentTime();
            eng->stopVoice(osc->voiceId, when);
        }
        return ev::undefined();
    });

    b.accessor("voiceId", [](Value self_, std::span<const Value>) {
        HostOscillatorNode* osc = oscOf(self_);
        return ev::fromDouble(osc ? osc->voiceId : -1);
    }, nullptr);

    b.def("setPeriodicWave", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostOscillatorNode* osc = oscOf(self_);
        if (!osc || a.empty()) return ev::undefined();
        HostPeriodicWave* pw = hostPeriodicWaveOf(a[0]);
        if (!pw) return ev::throwTypeError("setPeriodicWave: expected PeriodicWave");
        osc->type = "custom";
        auto* eng = getAudioEngine();
        if (eng && osc->voiceId >= 0 && pw->wavetable) {
            eng->setWaveform(osc->voiceId, broaudio::Waveform::Wavetable);
            eng->setVoiceWavetable(osc->voiceId, pw->wavetable);
        }
        return ev::undefined();
    });
}

Value makeOscillatorNodeValue() {
    auto* osc = new HostOscillatorNode();
    osc->base.nodeType = AudioNodeType::Oscillator;

    auto* eng = getAudioEngine();
    if (eng) {
        osc->voiceId = eng->createVoice();
        eng->setWaveform(osc->voiceId, broaudio::Waveform::Sine);
        eng->setFrequency(osc->voiceId, 440.0f);
    }

    ObjectBuilder b(g_oscillatorNodeClass.make(osc, hostOscillatorDtor));
    int voiceId = osc->voiceId;
    b.set("frequency", makeAudioParamValue(AudioParamTarget::VoiceFrequency, voiceId, 440.0f, 0.0f, 24000.0f, 440.0f));
    b.set("detune", makeAudioParamValue(AudioParamTarget::VoiceDetune, voiceId, 0.0f, -153600.0f, 153600.0f, 0.0f));
    // Beyond Web Audio: the oscillator is a synth voice, so its envelope,
    // pan, pitch bend and gain are params too (audio-api.js).
    b.set("pan", makeAudioParamValue(AudioParamTarget::VoicePan, voiceId, 0.0f, -1.0f, 1.0f, 0.0f));
    b.set("attack", makeAudioParamValue(AudioParamTarget::VoiceAttack, voiceId, 0.01f, 0.0f, 60.0f, 0.01f));
    b.set("decay", makeAudioParamValue(AudioParamTarget::VoiceDecay, voiceId, 0.1f, 0.0f, 60.0f, 0.1f));
    b.set("sustain", makeAudioParamValue(AudioParamTarget::VoiceSustain, voiceId, 1.0f, 0.0f, 1.0f, 1.0f));
    b.set("release", makeAudioParamValue(AudioParamTarget::VoiceRelease, voiceId, 0.04f, 0.0f, 60.0f, 0.04f));
    b.set("pitchBend", makeAudioParamValue(AudioParamTarget::VoicePitchBend, voiceId, 0.0f, -24.0f, 24.0f, 0.0f));
    b.set("gain", makeAudioParamValue(AudioParamTarget::Gain, voiceId, 1.0f, 0.0f, 10.0f, 1.0f));
    return b.get();
}

void decorateBiquadFilterNodeProto(ObjectBuilder& b) {
    // The filter slot runs while the node is connected; the shared
    // AudioNode.connect/disconnect toggle it (see decorateAudioNodeProto).
    b.accessor("type",
               [](Value self_, std::span<const Value>) {
                   HostBiquadFilterNode* filter = filterOf(self_);
                   if (!filter) return ev::undefined();
                   return ev::fromUtf8(filter->type);
               },
               [](Value self_, std::span<const Value> a) {
                   HostBiquadFilterNode* filter = filterOf(self_);
                   if (!filter || a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string t = ev::toUtf8(a[0]);
                   filter->type = t;
                   auto* eng = getAudioEngine();
                   if (eng && filter->slot >= 0) {
                       eng->setFilterType(filter->slot, parseFilterType(t));
                   }
                   return ev::undefined();
               });

    b.def("getFrequencyResponse", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostBiquadFilterNode* filter = filterOf(self_);
        if (!filter || a.size() < 3) return ev::undefined();

        // The param reads allocate, so they come first (off a rooted self);
        // the typed arrays' bytes are looked up only after the last of them.
        double f0 = 350.0;
        double Q = 1.0;
        double gainDb = 0.0;
        double detuneCents = 0.0;
        {
            ev::Persistent self(self_);
            if (auto* p = hostAudioParamOf(ev::getProperty(self.get(), "frequency"))) f0 = p->value;
            if (auto* p = hostAudioParamOf(ev::getProperty(self.get(), "Q"))) Q = p->value;
            if (auto* p = hostAudioParamOf(ev::getProperty(self.get(), "gain"))) gainDb = p->value;
            if (auto* p = hostAudioParamOf(ev::getProperty(self.get(), "detune"))) detuneCents = p->value;
        }
        // computedFrequency = frequency * 2^(detune / 1200) (Web Audio).
        if (detuneCents != 0.0) f0 *= std::pow(2.0, detuneCents / 1200.0);

        std::vector<float> freqStorage;
        const float* freqs = nullptr;
        size_t count = 0;
        if (!floatData(a[0], freqStorage, &freqs, &count) || count == 0) return ev::undefined();

        if (!ev::isTypedArray(a[1]) || !ev::isTypedArray(a[2])) return ev::undefined();
        ev::TypedArrayInfo magInfo = ev::typedArrayInfo(a[1]);
        ev::TypedArrayInfo phaseInfo = ev::typedArrayInfo(a[2]);
        if (!magInfo || !phaseInfo || !magInfo.data || !phaseInfo.data) return ev::undefined();

        auto* eng = getAudioEngine();
        int sr = eng ? eng->sampleRate() : 44100;

        // Compute RBJ filter coefficients
        double w0 = 2.0 * M_PI * f0 / sr;
        double alpha = std::sin(w0) / (2.0 * std::max(0.0001, Q));
        double A = std::pow(10.0, gainDb / 40.0);
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

        std::string type = filter->type;
        if (type == "lowpass") {
            b0 = (1.0 - std::cos(w0)) / 2.0;
            b1 = 1.0 - std::cos(w0);
            b2 = (1.0 - std::cos(w0)) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * std::cos(w0);
            a2 = 1.0 - alpha;
        } else if (type == "highpass") {
            b0 = (1.0 + std::cos(w0)) / 2.0;
            b1 = -(1.0 + std::cos(w0));
            b2 = (1.0 + std::cos(w0)) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * std::cos(w0);
            a2 = 1.0 - alpha;
        } else if (type == "bandpass") {
            b0 = alpha;
            b1 = 0.0;
            b2 = -alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * std::cos(w0);
            a2 = 1.0 - alpha;
        } else if (type == "notch") {
            b0 = 1.0;
            b1 = -2.0 * std::cos(w0);
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * std::cos(w0);
            a2 = 1.0 - alpha;
        } else if (type == "peaking") {
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * std::cos(w0);
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * std::cos(w0);
            a2 = 1.0 - alpha / A;
        } else if (type == "lowshelf") {
            double sqrtA = std::sqrt(A);
            b0 = A * ((A + 1.0) - (A - 1.0) * std::cos(w0) + 2.0 * sqrtA * alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * std::cos(w0));
            b2 = A * ((A + 1.0) - (A - 1.0) * std::cos(w0) - 2.0 * sqrtA * alpha);
            a0 = (A + 1.0) + (A - 1.0) * std::cos(w0) + 2.0 * sqrtA * alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * std::cos(w0));
            a2 = (A + 1.0) + (A - 1.0) * std::cos(w0) - 2.0 * sqrtA * alpha;
        } else if (type == "highshelf") {
            double sqrtA = std::sqrt(A);
            b0 = A * ((A + 1.0) + (A - 1.0) * std::cos(w0) + 2.0 * sqrtA * alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * std::cos(w0));
            b2 = A * ((A + 1.0) + (A - 1.0) * std::cos(w0) - 2.0 * sqrtA * alpha);
            a0 = (A + 1.0) - (A - 1.0) * std::cos(w0) + 2.0 * sqrtA * alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * std::cos(w0));
            a2 = (A + 1.0) - (A - 1.0) * std::cos(w0) - 2.0 * sqrtA * alpha;
        } else { // allpass
            b0 = 1.0 - alpha;
            b1 = -2.0 * std::cos(w0);
            b2 = 1.0 + alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * std::cos(w0);
            a2 = 1.0 - alpha;
        }

        // Normalize by a0
        b0 /= a0; b1 /= a0; b2 /= a0;
        a1 /= a0; a2 /= a0;

        size_t n = std::min({count, static_cast<size_t>(magInfo.elementCount), static_cast<size_t>(phaseInfo.elementCount)});
        float* magOut = reinterpret_cast<float*>(magInfo.data);
        float* phaseOut = reinterpret_cast<float*>(phaseInfo.data);

        for (size_t i = 0; i < n; ++i) {
            double w = 2.0 * M_PI * freqs[i] / sr;
            double cos_w = std::cos(w);
            double sin_w = std::sin(w);
            double cos_2w = std::cos(2.0 * w);
            double sin_2w = std::sin(2.0 * w);

            double num_r = b0 + b1 * cos_w + b2 * cos_2w;
            double num_i = -b1 * sin_w - b2 * sin_2w;
            double den_r = 1.0 + a1 * cos_w + a2 * cos_2w;
            double den_i = -a1 * sin_w - a2 * sin_2w;

            double den_mag2 = den_r * den_r + den_i * den_i;
            if (den_mag2 > 1e-12) {
                double r = (num_r * den_r + num_i * den_i) / den_mag2;
                double im = (num_i * den_r - num_r * den_i) / den_mag2;
                magOut[i] = static_cast<float>(std::sqrt(r * r + im * im));
                phaseOut[i] = static_cast<float>(std::atan2(im, r));
            } else {
                magOut[i] = 1.0f;
                phaseOut[i] = 0.0f;
            }
        }

        return ev::undefined();
    });
}

Value makeBiquadFilterNodeValue() {
    auto* filter = new HostBiquadFilterNode();
    filter->base.nodeType = AudioNodeType::BiquadFilter;

    auto* eng = getAudioEngine();
    if (eng) {
        filter->slot = eng->allocateFilterSlot();
        if (filter->slot < 0) {
            delete filter;
            return ev::throwError("No filter slots available");
        }
        eng->setFilterEnabled(filter->slot, true);
        eng->setFilterType(filter->slot, broaudio::BiquadFilter::Type::Lowpass);
        eng->setFilterFrequency(filter->slot, 350.0f);
        eng->setFilterQ(filter->slot, 1.0f);
        eng->setFilterGain(filter->slot, 0.0f);
    }

    ObjectBuilder b(g_biquadFilterNodeClass.make(filter, hostBiquadFilterDtor));
    b.set("frequency", makeAudioParamValue(AudioParamTarget::FilterFrequency, filter->slot, 350.0f, 0.0f, 24000.0f, 350.0f));
    b.set("detune", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -153600.0f, 153600.0f, 0.0f));
    b.set("Q", makeAudioParamValue(AudioParamTarget::FilterQ, filter->slot, 1.0f, 0.0001f, 1000.0f, 1.0f));
    b.set("gain", makeAudioParamValue(AudioParamTarget::FilterGain, filter->slot, 0.0f, -40.0f, 40.0f, 0.0f));
    return b.get();
}

// The latest `n` samples of what the analyser taps (source: 0 output,
// 1 mic, 2 both), into `dst`.
static void readAnalyserSource(const HostAnalyserNode* analyser, float* dst, int n) {
    auto* e = getAudioEngine();
    if (!e || n <= 0) return;
    if (analyser->source == 2) {
        if (analyser->hasConnectedInput && analyser->inputTapBuffer) {
            analyser->inputTapBuffer->readLatest(dst, n);
        } else {
            e->outputBuffer().readLatest(dst, n);
        }
        if (!e->isMicMuted()) {
            std::vector<float> mic(static_cast<size_t>(n), 0.0f);
            e->micBuffer().readLatest(mic.data(), n);
            for (int i = 0; i < n; i++) dst[i] += mic[i];
        }
        return;
    }
    if (analyser->source == 1) {
        e->micBuffer().readLatest(dst, n);
        return;
    }
    if (analyser->hasConnectedInput && analyser->inputTapBuffer) {
        analyser->inputTapBuffer->readLatest(dst, n);
    } else {
        e->outputBuffer().readLatest(dst, n);
    }
}

void decorateMediaStreamSourceNodeProto(ObjectBuilder&) {
    // connect(analyser) -> analyser.source = 1 is handled by the shared
    // AudioNode.connect (see decorateAudioNodeProto).
}

void decorateAnalyserNodeProto(ObjectBuilder& b) {
    b.accessor("source",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->source);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   int v = i32At(a, 0);
                   analyser->source = (v == 2) ? 2 : (v == 1) ? 1 : 0;
                   return ev::undefined();
               });

    b.accessor("fftSize",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->fftSize);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   int sz = i32At(a, 0);
                   if (sz >= 32 && sz <= 32768 && (sz & (sz - 1)) == 0) {
                       analyser->fftSize = sz;
                   } else {
                       ev::throwRangeError("AnalyserNode.fftSize must be a power of 2 between 32 and 32768");
                   }
                   return ev::undefined();
               });

    b.accessor("frequencyBinCount", [](Value self_, std::span<const Value>) {
        HostAnalyserNode* analyser = analyserOf(self_);
        return ev::fromDouble(analyser ? analyser->fftSize / 2 : 1024);
    }, nullptr);

    b.accessor("minDecibels",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->minDecibels);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   analyser->minDecibels = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("maxDecibels",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->maxDecibels);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   analyser->maxDecibels = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("smoothingTimeConstant",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->smoothingTimeConstant);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   analyser->smoothingTimeConstant = static_cast<float>(std::clamp(numAt(a, 0), 0.0, 1.0));
                   return ev::undefined();
               });

    b.def("getFloatFrequencyData", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAnalyserNode* analyser = analyserOf(self_);
        if (!analyser) return ev::undefined();
        if (a.empty() || !ev::isTypedArray(a[0])) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
        if (!info || !info.data) return ev::undefined();

        int n = analyser->fftSize;
        int halfN = n / 2;
        std::vector<float> real(n, 0.0f), imag(n, 0.0f);
        readAnalyserSource(analyser, real.data(), n);

        for (int i = 0; i < n; i++) {
            float w = 0.42f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                            + 0.08f * std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
            real[i] *= w;
        }

        broaudio::fft(real.data(), imag.data(), n);

        if (analyser->smoothedMagnitudes.size() != static_cast<size_t>(halfN)) {
            analyser->smoothedMagnitudes.assign(halfN, -100.0f);
        }

        float sm = std::clamp(analyser->smoothingTimeConstant, 0.0f, 1.0f);
        std::vector<float> outData(halfN);
        for (int i = 0; i < halfN; i++) {
            float mag = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / (n / 2.0f);
            float db = (mag > 1e-6f) ? 20.0f * std::log10(mag) : -100.0f;
            analyser->smoothedMagnitudes[i] = sm * analyser->smoothedMagnitudes[i] + (1.0f - sm) * db;
            outData[i] = analyser->smoothedMagnitudes[i];
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(halfN));
        std::memcpy(info.data, outData.data(), count * sizeof(float));
        return ev::undefined();
    });

    b.def("getByteFrequencyData", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAnalyserNode* analyser = analyserOf(self_);
        if (!analyser) return ev::undefined();
        if (a.empty() || !ev::isTypedArray(a[0])) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
        if (!info || !info.data) return ev::undefined();

        int n = analyser->fftSize;
        int halfN = n / 2;
        std::vector<float> real(n, 0.0f), imag(n, 0.0f);
        readAnalyserSource(analyser, real.data(), n);

        for (int i = 0; i < n; i++) {
            float w = 0.42f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                            + 0.08f * std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
            real[i] *= w;
        }

        broaudio::fft(real.data(), imag.data(), n);

        if (analyser->smoothedMagnitudes.size() != static_cast<size_t>(halfN)) {
            analyser->smoothedMagnitudes.assign(halfN, -100.0f);
        }

        float sm = std::clamp(analyser->smoothingTimeConstant, 0.0f, 1.0f);
        float minDb = analyser->minDecibels;
        float maxDb = analyser->maxDecibels;
        float range = (maxDb > minDb) ? (maxDb - minDb) : 1.0f;

        std::vector<uint8_t> outData(halfN);
        for (int i = 0; i < halfN; i++) {
            float mag = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / (n / 2.0f);
            float db = (mag > 1e-6f) ? 20.0f * std::log10(mag) : -100.0f;
            analyser->smoothedMagnitudes[i] = sm * analyser->smoothedMagnitudes[i] + (1.0f - sm) * db;
            float norm = (analyser->smoothedMagnitudes[i] - minDb) / range;
            norm = std::clamp(norm, 0.0f, 1.0f);
            outData[i] = static_cast<uint8_t>(norm * 255.0f);
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(halfN));
        std::memcpy(info.data, outData.data(), count);
        return ev::undefined();
    });

    b.def("getFloatTimeDomainData", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAnalyserNode* analyser = analyserOf(self_);
        if (!analyser) return ev::undefined();
        if (a.empty() || !ev::isTypedArray(a[0])) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
        if (!info || !info.data) return ev::undefined();

        int n = analyser->fftSize;
        std::vector<float> real(n, 0.0f);
        readAnalyserSource(analyser, real.data(), n);

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(n));
        std::memcpy(info.data, real.data(), count * sizeof(float));
        return ev::undefined();
    });

    b.def("getByteTimeDomainData", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAnalyserNode* analyser = analyserOf(self_);
        if (!analyser) return ev::undefined();
        if (a.empty() || !ev::isTypedArray(a[0])) return ev::undefined();
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
        if (!info || !info.data) return ev::undefined();

        int n = analyser->fftSize;
        std::vector<float> real(n, 0.0f);
        readAnalyserSource(analyser, real.data(), n);

        std::vector<uint8_t> outData(n);
        for (int i = 0; i < n; i++) {
            float s = std::clamp(real[i], -1.0f, 1.0f);
            outData[i] = static_cast<uint8_t>((s * 0.5f + 0.5f) * 255.0f);
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(n));
        std::memcpy(info.data, outData.data(), count);
        return ev::undefined();
    });
}

Value makeAnalyserNodeValue() {
    auto* analyser = new HostAnalyserNode();
    analyser->base.nodeType = AudioNodeType::Analyser;
    analyser->inputTapBuffer = std::make_shared<broaudio::AnalysisBuffer>(16384);
    return g_analyserNodeClass.make(analyser, hostAnalyserDtor);
}

} // namespace broaudio::api
