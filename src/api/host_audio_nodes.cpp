#include "host_audio_internal.h"

namespace broaudio::api {

HostClass g_audioNodeClass;
HostClass g_audioParamClass;
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

void hostAudioParamDtor(void* p) {
    delete static_cast<HostAudioParam*>(p);
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

HostAudioParam* hostAudioParamOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioParam*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioParamTag) return nullptr;
    return p;
}

HostPeriodicWave* hostPeriodicWaveOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostPeriodicWave*>(ev::handleData(v));
    if (!p || p->tag != kHostPeriodicWaveTag) return nullptr;
    return p;
}

void syncAudioParamValue(HostAudioParam* p, float val) {
    p->value = std::clamp(val, p->minValue, p->maxValue);
    auto* e = getAudioEngine();
    if (!e || p->targetId < 0) return;
    switch (p->target) {
        case AudioParamTarget::VoiceFrequency:
            e->setFrequency(p->targetId, p->value);
            break;
        case AudioParamTarget::VoicePan:
        case AudioParamTarget::Pan:
            e->setVoicePan(p->targetId, p->value);
            break;
        case AudioParamTarget::FilterFrequency:
            e->setFilterFrequency(p->targetId, p->value);
            break;
        case AudioParamTarget::FilterQ:
            e->setFilterQ(p->targetId, p->value);
            break;
        case AudioParamTarget::FilterGain:
            e->setFilterGain(p->targetId, p->value);
            break;
        case AudioParamTarget::PlaybackRate:
            e->setPlaybackRate(p->targetId, p->value);
            break;
        case AudioParamTarget::DelayTime:
            e->setDelayTime(p->value);
            break;
        case AudioParamTarget::Gain:
            e->setGain(p->targetId, p->value);
            break;
        case AudioParamTarget::VoiceAttack:
            e->setAttackTime(p->targetId, p->value);
            break;
        case AudioParamTarget::VoiceDecay:
            e->setDecayTime(p->targetId, p->value);
            break;
        case AudioParamTarget::VoiceSustain:
            e->setSustainLevel(p->targetId, p->value);
            break;
        case AudioParamTarget::VoiceRelease:
            e->setReleaseTime(p->targetId, p->value);
            break;
        case AudioParamTarget::VoicePitchBend:
            e->setVoicePitchBend(p->targetId, p->value);
            break;
        case AudioParamTarget::VoiceDetune:
        case AudioParamTarget::PlaybackDetune:
        case AudioParamTarget::PannerPositionX:
        case AudioParamTarget::PannerPositionY:
        case AudioParamTarget::PannerPositionZ:
        case AudioParamTarget::PannerOrientationX:
        case AudioParamTarget::PannerOrientationY:
        case AudioParamTarget::PannerOrientationZ:
        case AudioParamTarget::CompressorThreshold:
        case AudioParamTarget::CompressorKnee:
        case AudioParamTarget::CompressorRatio:
        case AudioParamTarget::CompressorAttack:
        case AudioParamTarget::CompressorRelease:
        case AudioParamTarget::Generic:
        default:
            break;
    }
}

void decorateAudioParamProto(ObjectBuilder& b) {
    b.accessor("value",
               [](Value self_, std::span<const Value>) {
                   HostAudioParam* p = hostAudioParamOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->value);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAudioParam* p = hostAudioParamOf(self_);
                   if (!p) return ev::undefined();
                   syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
                   return ev::undefined();
               });

    b.accessor("defaultValue", [](Value self_, std::span<const Value>) {
        HostAudioParam* p = hostAudioParamOf(self_);
        return ev::fromDouble(p ? p->defaultValue : 1.0);
    }, nullptr);

    b.accessor("minValue", [](Value self_, std::span<const Value>) {
        HostAudioParam* p = hostAudioParamOf(self_);
        return ev::fromDouble(p ? p->minValue : -3.4e38f);
    }, nullptr);

    b.accessor("maxValue", [](Value self_, std::span<const Value>) {
        HostAudioParam* p = hostAudioParamOf(self_);
        return ev::fromDouble(p ? p->maxValue : 3.4e38f);
    }, nullptr);

    b.def("setValueAtTime", 2, [](Value self_, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(self_);
        if (p && !a.empty()) syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return self_;
    });

    b.def("linearRampToValueAtTime", 2, [](Value self_, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(self_);
        if (p && !a.empty()) syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return self_;
    });

    b.def("exponentialRampToValueAtTime", 2, [](Value self_, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(self_);
        if (p && !a.empty()) syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return self_;
    });

    b.def("setTargetAtTime", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(self_);
        if (p && !a.empty()) syncAudioParamValue(p, static_cast<float>(numAt(a, 0)));
        return self_;
    });

    b.def("setValueCurveAtTime", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostAudioParam* p = hostAudioParamOf(self_);
        if (p && !a.empty()) {
            std::vector<float> storage;
            const float* data = nullptr;
            size_t count = 0;
            if (floatData(a[0], storage, &data, &count) && count > 0) {
                syncAudioParamValue(p, data[count - 1]);
            }
        }
        return self_;
    });

    b.def("cancelScheduledValues", 1, [](Value self_, std::span<const Value>) -> Value {
        return self_;
    });

    b.def("cancelAndHoldAtTime", 1, [](Value self_, std::span<const Value>) -> Value {
        return self_;
    });
}

Value makeAudioParamValue(AudioParamTarget target, int targetId,
                          float initialVal, float minVal, float maxVal, float defaultVal) {
    auto* param = new HostAudioParam();
    param->target = target;
    param->targetId = targetId;
    param->value = initialVal;
    param->defaultValue = defaultVal;
    param->minValue = minVal;
    param->maxValue = maxVal;
    return g_audioParamClass.make(param, hostAudioParamDtor);
}

void decorateAudioNodeProto(ObjectBuilder& b) {
    b.def("connect", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("AudioNode.connect: destination argument required");
        return a[0];
    });

    b.def("disconnect", 1, [](Value, std::span<const Value>) -> Value {
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
    ObjectBuilder b(g_gainNodeClass.make(gain, hostGainDtor));
    Value gainParam = makeAudioParamValue(AudioParamTarget::Gain, -1, 1.0f, -3.4e38f, 3.4e38f, 1.0f);
    b.set("gain", gainParam);
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

    b.def("start", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostOscillatorNode* osc = oscOf(self_);
        if (!osc) return ev::undefined();
        if (osc->started) return ev::throwError("OscillatorNode cannot be started more than once");
        osc->started = true;
        auto* eng = getAudioEngine();
        if (eng && osc->voiceId >= 0) {
            eng->startVoice(osc->voiceId, numAt(a, 0));
        }
        return ev::undefined();
    });

    b.def("stop", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostOscillatorNode* osc = oscOf(self_);
        if (!osc) return ev::undefined();
        osc->stopped = true;
        auto* eng = getAudioEngine();
        if (eng && osc->voiceId >= 0) {
            eng->stopVoice(osc->voiceId, numAt(a, 0));
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
    b.set("frequency", makeAudioParamValue(AudioParamTarget::VoiceFrequency, osc->voiceId, 440.0f, 0.0f, 24000.0f, 440.0f));
    b.set("detune", makeAudioParamValue(AudioParamTarget::VoiceDetune, osc->voiceId, 0.0f, -153600.0f, 153600.0f, 0.0f));
    return b.get();
}

void decorateBiquadFilterNodeProto(ObjectBuilder& b) {
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

        std::vector<float> freqStorage;
        const float* freqs = nullptr;
        size_t count = 0;
        if (!floatData(a[0], freqStorage, &freqs, &count) || count == 0) return ev::undefined();

        Value magArr = a[1];
        Value phaseArr = a[2];
        if (!ev::isTypedArray(magArr) || !ev::isTypedArray(phaseArr)) return ev::undefined();
        ev::TypedArrayInfo magInfo = ev::typedArrayInfo(magArr);
        ev::TypedArrayInfo phaseInfo = ev::typedArrayInfo(phaseArr);
        if (!magInfo || !phaseInfo || !magInfo.data || !phaseInfo.data) return ev::undefined();

        auto* eng = getAudioEngine();
        int sr = eng ? eng->sampleRate() : 44100;

        Value freqVal = ev::getProperty(self_, "frequency");
        Value qVal = ev::getProperty(self_, "Q");
        Value gainVal = ev::getProperty(self_, "gain");

        double f0 = 350.0;
        double Q = 1.0;
        double gainDb = 0.0;
        if (auto* p = hostAudioParamOf(freqVal)) f0 = p->value;
        if (auto* p = hostAudioParamOf(qVal)) Q = p->value;
        if (auto* p = hostAudioParamOf(gainVal)) gainDb = p->value;

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
        if (filter->slot >= 0) {
            eng->setFilterEnabled(filter->slot, true);
            eng->setFilterType(filter->slot, broaudio::BiquadFilter::Type::Lowpass);
            eng->setFilterFrequency(filter->slot, 350.0f);
            eng->setFilterQ(filter->slot, 1.0f);
            eng->setFilterGain(filter->slot, 0.0f);
        }
    }

    ObjectBuilder b(g_biquadFilterNodeClass.make(filter, hostBiquadFilterDtor));
    b.set("frequency", makeAudioParamValue(AudioParamTarget::FilterFrequency, filter->slot, 350.0f, 0.0f, 24000.0f, 350.0f));
    b.set("detune", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -153600.0f, 153600.0f, 0.0f));
    b.set("Q", makeAudioParamValue(AudioParamTarget::FilterQ, filter->slot, 1.0f, 0.0001f, 1000.0f, 1.0f));
    b.set("gain", makeAudioParamValue(AudioParamTarget::FilterGain, filter->slot, 0.0f, -40.0f, 40.0f, 0.0f));
    return b.get();
}

void decorateAnalyserNodeProto(ObjectBuilder& b) {
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
        auto* e = getAudioEngine();
        if (e) {
            e->outputBuffer().readLatest(real.data(), n);
        }

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
        auto* e = getAudioEngine();
        if (e) {
            e->outputBuffer().readLatest(real.data(), n);
        }

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
        auto* e = getAudioEngine();
        if (e) {
            e->outputBuffer().readLatest(real.data(), n);
        }

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
        auto* e = getAudioEngine();
        if (e) {
            e->outputBuffer().readLatest(real.data(), n);
        }

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
    return g_analyserNodeClass.make(analyser, hostAnalyserDtor);
}

} // namespace broaudio::api
