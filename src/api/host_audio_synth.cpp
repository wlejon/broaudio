#include "host_audio_internal.h"
#include <map>

namespace broaudio::api {

HostClass g_voiceAllocatorClass;
HostClass g_modMatrixClass;
HostClass g_midiInputClass;
HostClass g_sequenceClass;
HostClass g_mediaStreamClass;
HostClass g_mediaStreamAudioSourceNodeClass;

static std::map<int, std::shared_ptr<broaudio::WavetableBank>> s_wavetables;
static int s_nextWavetableId = 1;

std::shared_ptr<broaudio::WavetableBank> findWavetable(int id) {
    auto it = s_wavetables.find(id);
    return it != s_wavetables.end() ? it->second : nullptr;
}

int registerWavetable(std::shared_ptr<broaudio::WavetableBank> bank) {
    if (!bank) return -1;
    int id = s_nextWavetableId++;
    s_wavetables[id] = std::move(bank);
    return id;
}

void deleteWavetable(int id) {
    s_wavetables.erase(id);
}

broaudio::DistortionMode parseDistortionMode(const std::string& str) {
    if (str == "hardclip") return broaudio::DistortionMode::HardClip;
    if (str == "foldback") return broaudio::DistortionMode::Foldback;
    if (str == "bitcrush") return broaudio::DistortionMode::Bitcrush;
    return broaudio::DistortionMode::SoftClip;
}

const char* distortionModeToString(broaudio::DistortionMode mode) {
    switch (mode) {
        case broaudio::DistortionMode::HardClip: return "hardclip";
        case broaudio::DistortionMode::Foldback: return "foldback";
        case broaudio::DistortionMode::Bitcrush: return "bitcrush";
        default: return "softclip";
    }
}

broaudio::LfoShape parseLfoShape(const std::string& str) {
    if (str == "triangle") return broaudio::LfoShape::Triangle;
    if (str == "square") return broaudio::LfoShape::Square;
    if (str == "sawup") return broaudio::LfoShape::SawUp;
    if (str == "sawdown") return broaudio::LfoShape::SawDown;
    if (str == "sampleandhold") return broaudio::LfoShape::SampleAndHold;
    return broaudio::LfoShape::Sine;
}

const char* lfoShapeToString(broaudio::LfoShape shape) {
    switch (shape) {
        case broaudio::LfoShape::Triangle: return "triangle";
        case broaudio::LfoShape::Square: return "square";
        case broaudio::LfoShape::SawUp: return "sawup";
        case broaudio::LfoShape::SawDown: return "sawdown";
        case broaudio::LfoShape::SampleAndHold: return "sampleandhold";
        default: return "sine";
    }
}

broaudio::ModSource parseModSource(const std::string& str) {
    if (str == "lfo1") return broaudio::ModSource::Lfo1;
    if (str == "lfo2") return broaudio::ModSource::Lfo2;
    if (str == "lfo3") return broaudio::ModSource::Lfo3;
    if (str == "lfo4") return broaudio::ModSource::Lfo4;
    if (str == "envelope") return broaudio::ModSource::Envelope;
    if (str == "velocity") return broaudio::ModSource::Velocity;
    if (str == "keytracking") return broaudio::ModSource::KeyTracking;
    if (str == "modwheel") return broaudio::ModSource::ModWheel;
    if (str == "aftertouch") return broaudio::ModSource::Aftertouch;
    return broaudio::ModSource::Lfo1;
}

const char* modSourceToString(broaudio::ModSource src) {
    switch (src) {
        case broaudio::ModSource::Lfo1: return "lfo1";
        case broaudio::ModSource::Lfo2: return "lfo2";
        case broaudio::ModSource::Lfo3: return "lfo3";
        case broaudio::ModSource::Lfo4: return "lfo4";
        case broaudio::ModSource::Envelope: return "envelope";
        case broaudio::ModSource::Velocity: return "velocity";
        case broaudio::ModSource::KeyTracking: return "keytracking";
        case broaudio::ModSource::ModWheel: return "modwheel";
        case broaudio::ModSource::Aftertouch: return "aftertouch";
        default: return "lfo1";
    }
}

broaudio::ModDest parseModDest(const std::string& str) {
    if (str == "gain") return broaudio::ModDest::Gain;
    if (str == "pan") return broaudio::ModDest::Pan;
    if (str == "filterfreq") return broaudio::ModDest::FilterFreq;
    if (str == "filterq") return broaudio::ModDest::FilterQ;
    if (str == "pulsewidth") return broaudio::ModDest::PulseWidth;
    if (str == "delaysend") return broaudio::ModDest::DelaySend;
    return broaudio::ModDest::Pitch;
}

const char* modDestToString(broaudio::ModDest dst) {
    switch (dst) {
        case broaudio::ModDest::Gain: return "gain";
        case broaudio::ModDest::Pan: return "pan";
        case broaudio::ModDest::FilterFreq: return "filterfreq";
        case broaudio::ModDest::FilterQ: return "filterq";
        case broaudio::ModDest::PulseWidth: return "pulsewidth";
        case broaudio::ModDest::DelaySend: return "delaysend";
        default: return "pitch";
    }
}

broaudio::DistanceModel parseDistanceModel(const std::string& str) {
    if (str == "linear") return broaudio::DistanceModel::Linear;
    if (str == "exponential") return broaudio::DistanceModel::Exponential;
    return broaudio::DistanceModel::Inverse;
}

const char* distanceModelToString(broaudio::DistanceModel model) {
    switch (model) {
        case broaudio::DistanceModel::Linear: return "linear";
        case broaudio::DistanceModel::Exponential: return "exponential";
        default: return "inverse";
    }
}

broaudio::EffectSlot parseEffectSlot(const std::string& str, broaudio::EffectSlot def) {
    if (str == "filter") return broaudio::EffectSlot::Filter;
    if (str == "delay") return broaudio::EffectSlot::Delay;
    if (str == "compressor") return broaudio::EffectSlot::Compressor;
    if (str == "chorus") return broaudio::EffectSlot::Chorus;
    if (str == "reverb") return broaudio::EffectSlot::Reverb;
    if (str == "equalizer" || str == "eq") return broaudio::EffectSlot::Equalizer;
    if (str == "distortion") return broaudio::EffectSlot::Distortion;
    return def;
}

const char* effectSlotToString(broaudio::EffectSlot slot) {
    switch (slot) {
        case broaudio::EffectSlot::Filter: return "filter";
        case broaudio::EffectSlot::Delay: return "delay";
        case broaudio::EffectSlot::Compressor: return "compressor";
        case broaudio::EffectSlot::Chorus: return "chorus";
        case broaudio::EffectSlot::Reverb: return "reverb";
        case broaudio::EffectSlot::Equalizer: return "equalizer";
        case broaudio::EffectSlot::Distortion: return "distortion";
        default: return "unknown";
    }
}

broaudio::StealPolicy parseStealPolicy(const std::string& str) {
    if (str == "quietest") return broaudio::StealPolicy::Quietest;
    if (str == "samenote") return broaudio::StealPolicy::SameNote;
    if (str == "none") return broaudio::StealPolicy::None;
    return broaudio::StealPolicy::Oldest;
}

void hostVoiceAllocatorDtor(void* p) {
    auto* h = static_cast<HostVoiceAllocator*>(p);
    delete h;
}

void hostModMatrixDtor(void* p) {
    delete static_cast<HostModMatrix*>(p);
}

void hostMidiInputDtor(void* p) {
    auto* h = static_cast<HostMidiInput*>(p);
    delete h;
}

void hostSequenceDtor(void* p) {
    delete static_cast<HostSequence*>(p);
}

void hostMediaStreamDtor(void* p) {
    delete static_cast<HostMediaStream*>(p);
}

void hostMediaStreamAudioSourceNodeDtor(void* p) {
    delete static_cast<HostMediaStreamAudioSourceNode*>(p);
}

HostVoiceAllocator* hostVoiceAllocatorOf(Value v) {
    auto* h = static_cast<HostVoiceAllocator*>(g_voiceAllocatorClass.unwrap(v));
    return (h && h->tag == kHostVoiceAllocatorTag) ? h : nullptr;
}

HostModMatrix* hostModMatrixOf(Value v) {
    auto* h = static_cast<HostModMatrix*>(g_modMatrixClass.unwrap(v));
    return (h && h->tag == kHostModMatrixTag) ? h : nullptr;
}

HostMidiInput* hostMidiInputOf(Value v) {
    auto* h = static_cast<HostMidiInput*>(g_midiInputClass.unwrap(v));
    return (h && h->tag == kHostMidiInputTag) ? h : nullptr;
}

HostSequence* hostSequenceOf(Value v) {
    auto* h = static_cast<HostSequence*>(g_sequenceClass.unwrap(v));
    return (h && h->tag == kHostSequenceTag) ? h : nullptr;
}

HostMediaStream* hostMediaStreamOf(Value v) {
    auto* h = static_cast<HostMediaStream*>(g_mediaStreamClass.unwrap(v));
    return (h && h->tag == kHostMediaStreamTag) ? h : nullptr;
}

// Only a MediaStreamAudioSourceNode's payload: the base tag alone would
// accept any node (every node leads with the same HostAudioNode).
HostMediaStreamAudioSourceNode* hostMediaStreamNodeOf(Value v) {
    return static_cast<HostMediaStreamAudioSourceNode*>(
        g_mediaStreamAudioSourceNodeClass.unwrap(v));
}

// ---------------------------------------------------------------------------
// VoiceAllocator
// ---------------------------------------------------------------------------

namespace {

// The JS voice-setup callback (setVoiceSetup) lives on the allocator object
// as `_voiceSetup`, where the collector traces it, rather than in a
// Persistent inside the finalized HostVoiceAllocator. Any path that can
// allocate voices — noteOn, a Sequence's update(), MidiInput.processEvents()
// — installs it on the C++ allocator for the duration of the call through
// this scope, so the callback fires for every voice the allocator hands out,
// the way the QuickJS binding's permanently-installed callback did. The
// callback is held in a Persistent: it runs JS, which allocates, and it may
// be called once per voice.
class ScopedVoiceSetup {
public:
    ScopedVoiceSetup(Value allocatorObj) {
        va_ = hostVoiceAllocatorOf(allocatorObj);
        if (!va_ || !va_->allocator) {
            va_ = nullptr;
            return;
        }
        auto cb = std::make_shared<ev::Persistent>(ev::getProperty(allocatorObj, "_voiceSetup"));
        if (!ev::isFunction(cb->get())) {
            va_->allocator->setVoiceSetup(nullptr);
            return;
        }
        va_->allocator->setVoiceSetup([cb](int voiceId, int note, float vel) {
            const Value args[3] = {ev::fromDouble(voiceId), ev::fromDouble(note), ev::fromDouble(vel)};
            ev::call(cb->get(), ev::undefined(), std::span<const Value>(args, 3));
        });
    }
    ~ScopedVoiceSetup() {
        if (va_ && va_->allocator) va_->allocator->setVoiceSetup(nullptr);
    }
    ScopedVoiceSetup(const ScopedVoiceSetup&) = delete;
    ScopedVoiceSetup& operator=(const ScopedVoiceSetup&) = delete;

private:
    HostVoiceAllocator* va_ = nullptr;
};

// The sequence whose update() is running on this thread. Automation lanes
// only fire from inside Sequence::update, so a lane's C++ closure finds its
// JS callback through this -- `_laneCbs["k<key>"]` on the sequence object --
// instead of holding it as a host root.
thread_local const ev::Persistent* t_updatingSequence = nullptr;

void callLaneCallback(int key, float val) {
    if (!t_updatingSequence) return;
    ev::Persistent cbs(ev::getProperty(t_updatingSequence->get(), "_laneCbs"));
    if (!ev::isObject(cbs.get())) return;
    ev::Persistent cb(ev::getProperty(cbs.get(), sequenceLaneKey(key)));
    if (!ev::isFunction(cb.get())) return;
    const Value arg = ev::fromDouble(val);
    ev::call(cb.get(), ev::undefined(), std::span<const Value>(&arg, 1));
}

// MidiRawEvent.type: the lowercase label the QuickJS binding (and
// audio-api.js) use, not the enum's number.
const char* midiEventTypeName(broaudio::MidiEvent::Type t) {
    switch (t) {
        case broaudio::MidiEvent::Type::NoteOn:          return "noteon";
        case broaudio::MidiEvent::Type::NoteOff:         return "noteoff";
        case broaudio::MidiEvent::Type::ControlChange:   return "controlchange";
        case broaudio::MidiEvent::Type::PitchBend:       return "pitchbend";
        case broaudio::MidiEvent::Type::ProgramChange:   return "programchange";
        case broaudio::MidiEvent::Type::Aftertouch:      return "aftertouch";
        case broaudio::MidiEvent::Type::ChannelPressure: return "channelpressure";
    }
    return "unknown";
}

}  // namespace

static void decorateVoiceAllocatorProto(ObjectBuilder& b) {
    b.def("noteOn", 3, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator && a.size() >= 2) {
            ScopedVoiceSetup setup(self);
            auto* e = getAudioEngine();
            double when = a.size() >= 3 ? numAt(a, 2) : (e ? e->currentTime() : 0.0);
            int voice = h->allocator->noteOn(i32At(a, 0), static_cast<float>(numAt(a, 1)), when);
            return ev::fromDouble(voice);
        }
        return ev::fromDouble(-1);
    });

    b.def("noteOff", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator && !a.empty()) {
            auto* e = getAudioEngine();
            double when = a.size() >= 2 ? numAt(a, 1) : (e ? e->currentTime() : 0.0);
            h->allocator->noteOff(i32At(a, 0), when);
        }
        return ev::undefined();
    });

    b.def("allNotesOff", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator) {
            auto* e = getAudioEngine();
            double when = !a.empty() ? numAt(a, 0) : (e ? e->currentTime() : 0.0);
            h->allocator->allNotesOff(when);
        }
        return ev::undefined();
    });

    b.def("setStealPolicy", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator && !a.empty()) {
            h->allocator->setStealPolicy(parseStealPolicy(ev::toUtf8(a[0])));
        }
        return ev::undefined();
    });

    b.def("setMaxVoices", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator && !a.empty()) {
            h->allocator->setMaxVoices(i32At(a, 0));
        }
        return ev::undefined();
    });

    b.def("setVoiceSetup", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (!h || !h->allocator) return ev::undefined();
        if (!a.empty() && ev::isFunction(a[0])) {
            ev::setProperty(self, "_voiceSetup", a[0]);
        } else {
            ev::setProperty(self, "_voiceSetup", ev::undefined());
        }
        return ev::undefined();
    });

    b.def("voiceForNote", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        return ev::fromDouble(h && h->allocator && !a.empty() ? h->allocator->voiceForNote(i32At(a, 0)) : -1);
    });

    b.accessor("activeVoiceCount", [](Value self, std::span<const Value>) {
        auto* h = hostVoiceAllocatorOf(self);
        return ev::fromDouble(h && h->allocator ? h->allocator->activeVoiceCount() : 0);
    }, nullptr);
}

Value makeVoiceAllocatorValue(int maxVoices) {
    auto* h = new HostVoiceAllocator();
    auto* e = getAudioEngine();
    if (e) {
        h->allocator = std::make_unique<broaudio::VoiceAllocator>(*e, maxVoices > 0 ? maxVoices : 16);
    }
    return g_voiceAllocatorClass.make(h, hostVoiceAllocatorDtor);
}

// ---------------------------------------------------------------------------
// ModMatrix
// ---------------------------------------------------------------------------

static void decorateModMatrixProto(ObjectBuilder& b) {
    b.def("setLfoShape", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoShape(idx, parseLfoShape(ev::toUtf8(a[1])));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoRate", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoRate(idx, static_cast<float>(numAt(a, 1)));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoDepth", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoDepth(idx, static_cast<float>(numAt(a, 1)));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoOffset", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoOffset(idx, static_cast<float>(numAt(a, 1)));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoBipolar", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoBipolar(idx, boolAt(a, 1));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoSync", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoSync(idx, boolAt(a, 1));
            }
        }
        return ev::undefined();
    });

    b.def("addRoute", 3, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (!h || !h->matrix || a.size() < 3) return ev::fromDouble(-1);
        auto src = parseModSource(ev::toUtf8(a[0]));
        auto dst = parseModDest(ev::toUtf8(a[1]));
        float amount = static_cast<float>(numAt(a, 2));
        int id = h->matrix->addRoute(src, dst, amount);
        return ev::fromDouble(id);
    });

    b.def("removeRoute", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && !a.empty()) h->matrix->removeRoute(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setRouteAmount", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            h->matrix->setRouteAmount(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        }
        return ev::undefined();
    });

    b.def("setRouteEnabled", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            h->matrix->setRouteEnabled(i32At(a, 0), boolAt(a, 1));
        }
        return ev::undefined();
    });

    b.def("clearAllRoutes", 0, [](Value self, std::span<const Value>) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix) h->matrix->clearAllRoutes();
        return ev::undefined();
    });

    b.def("setModWheel", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && !a.empty()) h->matrix->setModWheel(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setAftertouch", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && !a.empty()) h->matrix->setAftertouch(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.accessor("routeCount", [](Value self, std::span<const Value>) {
        auto* h = hostModMatrixOf(self);
        return ev::fromDouble(h && h->matrix ? h->matrix->routeCount() : 0);
    }, nullptr);
}

Value makeModMatrixValue() {
    auto* h = new HostModMatrix();
    auto* e = getAudioEngine();
    if (e) h->matrix = &e->modMatrix();
    return g_modMatrixClass.make(h, hostModMatrixDtor);
}

// ---------------------------------------------------------------------------
// MidiInput
// ---------------------------------------------------------------------------

static void decorateMidiInputProto(ObjectBuilder& b) {
    b.def("availablePorts", 0, [](Value self, std::span<const Value>) -> Value {
        auto* h = hostMidiInputOf(self);
        if (!h || !h->midi) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        auto ports = h->midi->availablePorts();
        return hostArrayOf(ports.size(), [&ports](size_t i) -> Value {
            ObjectBuilder p;
            p.set("index", ev::fromDouble(ports[i].index));
            p.set("name", ev::fromUtf8(ports[i].name));
            return p.get();
        });
    });

    b.def("open", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && !a.empty()) return ev::fromBool(h->midi->open(i32At(a, 0)));
        return ev::fromBool(false);
    });

    b.def("close", 0, [](Value self, std::span<const Value>) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi) h->midi->close();
        return ev::undefined();
    });

    b.accessor("isOpen", [](Value self, std::span<const Value>) {
        auto* h = hostMidiInputOf(self);
        return ev::fromBool(h && h->midi ? h->midi->isOpen() : false);
    }, nullptr);

    b.def("connectToAllocator", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && !a.empty()) {
            auto* alloc = hostVoiceAllocatorOf(a[0]);
            h->midi->connectToAllocator(alloc ? alloc->allocator.get() : nullptr);
            ev::setProperty(self, "_connectedAllocator", a[0]);
        }
        return ev::undefined();
    });

    b.def("onControlChange", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && a.size() >= 2) {
            int cc = i32At(a, 0);
            if (cc >= 0 && cc < 128) {
                std::string prop = "_cc_" + std::to_string(cc);
                if (ev::isFunction(a[1])) {
                    ev::setProperty(self, prop.c_str(), a[1]);
                } else {
                    ev::setProperty(self, prop.c_str(), ev::undefined());
                }
            }
        }
        return ev::undefined();
    });

    b.def("onPitchBend", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && !a.empty()) {
            if (ev::isFunction(a[0])) {
                ev::setProperty(self, "_pitchBendCb", a[0]);
            } else {
                ev::setProperty(self, "_pitchBendCb", ev::undefined());
            }
        }
        return ev::undefined();
    });

    b.def("onRawEvent", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && !a.empty()) {
            if (ev::isFunction(a[0])) {
                ev::setProperty(self, "_rawCb", a[0]);
            } else {
                ev::setProperty(self, "_rawCb", ev::undefined());
            }
        }
        return ev::undefined();
    });

    // injectMessage(bytes, timestamp?) -> bool: feed one raw MIDI message
    // (array or typed array of status + data bytes) in as if it came from the
    // open port; the next processEvents() dispatches it. Works with no port
    // open, for tests and on-screen keyboards. false for system messages,
    // truncated messages, or a full queue.
    b.def("injectMessage", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (!h || !h->midi || a.empty()) return ev::fromBool(false);
        double when = hasArg(a, 1) ? numAt(a, 1) : -1.0;
        if (isDetachedBuffer(a[0])) {
            throwArrayTypeError(a[0], "MidiInput.injectMessage: bytes", "an array or typed array");
            return ev::undefined();
        }
        std::vector<uint8_t> bytes;
        ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
        if (info && info.bytesPerElement == 1) {
            bytes.assign(info.data, info.data + info.byteLength);
        } else {
            std::vector<double> storage;
            const double* data = nullptr;
            size_t count = 0;
            if (!plainArrayData<double>(a[0], storage, [](double d) { return d; }, &data, &count)) {
                return ev::fromBool(false);
            }
            bytes.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                bytes.push_back(static_cast<uint8_t>(saturateI32(data[i]) & 0xFF));
            }
        }
        return ev::fromBool(h->midi->injectMessage(bytes.data(), bytes.size(), when));
    });

    b.def("processEvents", 0, [](Value self_, std::span<const Value>) {
        auto* h = hostMidiInputOf(self_);
        if (!h || !h->midi) return ev::undefined();

        // Every read below may allocate (and every callback runs JS, which
        // does), so the receiver and each callback are held in Persistents;
        // a callback's closure owns its Persistent through a shared_ptr.
        ev::Persistent self(self_);
        auto rooted = [&](const std::string& key) {
            return std::make_shared<ev::Persistent>(ev::getProperty(self.get(), key));
        };

        auto rawCb = rooted("_rawCb");
        if (ev::isFunction(rawCb->get())) {
            h->midi->onRawEvent([rawCb](const broaudio::MidiEvent& ev) {
                ObjectBuilder evObj;
                evObj.set("type", midiEventTypeName(ev.type));
                evObj.set("channel", ev::fromDouble(ev.channel));
                evObj.set("data1", ev::fromDouble(ev.data1));
                evObj.set("data2", ev::fromDouble(ev.data2));
                evObj.set("pitchBend", ev::fromDouble(ev.pitchBend));
                evObj.set("timestamp", ev::fromDouble(ev.timestamp));
                const Value v = evObj.get();
                ev::call(rawCb->get(), ev::undefined(), std::span<const Value>(&v, 1));
            });
        } else {
            h->midi->onRawEvent(nullptr);
        }

        auto pbCb = rooted("_pitchBendCb");
        if (ev::isFunction(pbCb->get())) {
            h->midi->onPitchBend([pbCb](uint8_t ch, int16_t val) {
                const Value args[2] = { ev::fromDouble(ch), ev::fromDouble(val) };
                ev::call(pbCb->get(), ev::undefined(), std::span<const Value>(args, 2));
            });
        } else {
            h->midi->onPitchBend(nullptr);
        }

        for (int cc = 0; cc < 128; ++cc) {
            auto cb = rooted("_cc_" + std::to_string(cc));
            if (ev::isFunction(cb->get())) {
                h->midi->onControlChange(static_cast<uint8_t>(cc), [cb](uint8_t ch, uint8_t ccn, uint8_t val) {
                    const Value args[3] = { ev::fromDouble(ch), ev::fromDouble(ccn), ev::fromDouble(val) };
                    ev::call(cb->get(), ev::undefined(), std::span<const Value>(args, 3));
                });
            } else {
                h->midi->onControlChange(static_cast<uint8_t>(cc), nullptr);
            }
        }

        ev::Persistent allocVal(ev::getProperty(self.get(), "_connectedAllocator"));
        {
            ScopedVoiceSetup setup(allocVal.get());
            h->midi->processEvents();
        }

        h->midi->onRawEvent(nullptr);
        h->midi->onPitchBend(nullptr);
        for (int cc = 0; cc < 128; ++cc) {
            h->midi->onControlChange(static_cast<uint8_t>(cc), nullptr);
        }

        return ev::undefined();
    });
}

Value makeMidiInputValue() {
    auto* e = getAudioEngine();
    if (!e) return ev::null();
    auto* h = new HostMidiInput();
    h->midi = std::make_unique<broaudio::MidiInput>(*e);
    return g_midiInputClass.make(h, hostMidiInputDtor);
}

// ---------------------------------------------------------------------------
// Sequence
// ---------------------------------------------------------------------------

static void decorateSequenceProto(ObjectBuilder& b) {
    b.def("setBPM", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) {
            if (a.size() >= 2) h->seq->setBPM(numAt(a, 0), numAt(a, 1));
            else h->seq->setBPM(numAt(a, 0));
        }
        return ev::undefined();
    });

    b.accessor("bpm", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromDouble(h && h->seq ? h->seq->bpm() : 120.0);
    }, nullptr);

    b.def("setTimeSignature", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 2) h->seq->setTimeSignature(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("addNote", 4, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 4) {
            broaudio::NoteEvent ev;
            ev.beatPosition = numAt(a, 0);
            ev.note = i32At(a, 1);
            ev.velocity = static_cast<float>(numAt(a, 2));
            ev.duration = numAt(a, 3);
            h->seq->addNote(ev);
        }
        return ev::undefined();
    });

    b.def("removeNote", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) h->seq->removeNote(i32At(a, 0));
        return ev::undefined();
    });

    b.def("clearNotes", 0, [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) h->seq->clearNotes();
        return ev::undefined();
    });

    b.accessor("noteCount", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromDouble(h && h->seq ? h->seq->noteCount() : 0);
    }, nullptr);

    b.def("note", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* h = hostSequenceOf(self);
        if (!h || !h->seq || a.empty()) return ev::null();
        int idx = i32At(a, 0);
        if (idx < 0 || idx >= h->seq->noteCount()) return ev::null();
        const auto& ev = h->seq->note(idx);
        ObjectBuilder obj;
        obj.set("beat", ev::fromDouble(ev.beatPosition));
        obj.set("beatPosition", ev::fromDouble(ev.beatPosition));
        obj.set("note", ev::fromDouble(ev.note));
        obj.set("velocity", ev::fromDouble(ev.velocity));
        obj.set("duration", ev::fromDouble(ev.duration));
        return obj.get();
    });

    b.def("play", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            double when = !a.empty() ? numAt(a, 0) : (getAudioEngine() ? getAudioEngine()->currentTime() : 0.0);
            h->seq->play(when);
        }
        return ev::undefined();
    });

    b.def("stop", 0, [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) h->seq->stop();
        return ev::undefined();
    });

    b.def("pause", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            double when = !a.empty() ? numAt(a, 0) : (getAudioEngine() ? getAudioEngine()->currentTime() : 0.0);
            h->seq->pause(when);
        }
        return ev::undefined();
    });

    b.def("resume", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            double when = !a.empty() ? numAt(a, 0) : (getAudioEngine() ? getAudioEngine()->currentTime() : 0.0);
            h->seq->resume(when);
        }
        return ev::undefined();
    });

    b.accessor("playing", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromBool(h && h->seq ? h->seq->isPlaying() : false);
    }, nullptr);

    b.accessor("paused", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromBool(h && h->seq ? h->seq->isPaused() : false);
    }, nullptr);

    b.def("setLoopEnabled", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) h->seq->setLoopEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.accessor("loopEnabled", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromBool(h && h->seq ? h->seq->isLoopEnabled() : false);
    }, nullptr);

    b.def("setLoopRange", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 2) h->seq->setLoopRange(numAt(a, 0), numAt(a, 1));
        return ev::undefined();
    });

    b.def("currentBeat", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (!h || !h->seq) return ev::fromDouble(0.0);
        double when = !a.empty() ? numAt(a, 0) : (getAudioEngine() ? getAudioEngine()->currentTime() : 0.0);
        return ev::fromDouble(h->seq->currentBeat(when));
    });

    b.def("update", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            double when = !a.empty() ? numAt(a, 0) : (getAudioEngine() ? getAudioEngine()->currentTime() : 0.0);
            // The notes this update fires go through the allocator: run its
            // voice-setup callback for them. Lanes find their callbacks on
            // this sequence object.
            ev::Persistent seqObj(self);
            ev::Persistent allocatorObj(ev::getProperty(seqObj.get(), "_allocator"));
            ScopedVoiceSetup setup(allocatorObj.get());
            const ev::Persistent* prev = t_updatingSequence;
            t_updatingSequence = &seqObj;
            h->seq->update(when);
            t_updatingSequence = prev;
        }
        return ev::undefined();
    });

    b.def("addAutomationLane", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (!h || !h->seq || a.empty() || !ev::isFunction(a[0])) return ev::fromDouble(-1);
        ev::Persistent seqObj(self);
        int laneIdx = h->seq->automationLaneCount();
        int key = h->nextLaneKey++;
        ev::Persistent cbs(ev::getProperty(seqObj.get(), "_laneCbs"));
        if (!ev::isObject(cbs.get())) {
            cbs.set(ev::createObject());
            ev::setProperty(seqObj.get(), "_laneCbs", cbs.get());
        }
        ev::setProperty(cbs.get(), sequenceLaneKey(key), a[0]);
        h->laneKeys.push_back(key);
        h->seq->addAutomationLane([key](float val) { callLaneCallback(key, val); });
        return ev::fromDouble(laneIdx);
    });

    decorateSequenceAutomation(b);
}

Value makeSequenceValue(Value allocatorObj) {
    HostVoiceAllocator* va = hostVoiceAllocatorOf(allocatorObj);
    if (!va || !va->allocator) return ev::null();
    ev::Persistent alloc(allocatorObj);
    auto* h = new HostSequence();
    h->seq = std::make_unique<broaudio::Sequence>(*va->allocator);
    // The sequence drives the allocator by reference: keep the allocator
    // object (and so the C++ allocator) alive as long as the sequence.
    ObjectBuilder b(g_sequenceClass.make(h, hostSequenceDtor));
    b.set("_allocator", alloc.get());
    return b.get();
}

// ---------------------------------------------------------------------------
// MediaStream
// ---------------------------------------------------------------------------

Value makeMediaStreamValue() {
    auto* s = new HostMediaStream();
    ObjectBuilder b(g_mediaStreamClass.make(s, hostMediaStreamDtor));
    b.accessor("active", [](Value, std::span<const Value>) { return ev::fromBool(true); }, nullptr);
    return b.get();
}

Value makeMediaStreamAudioSourceNodeValue() {
    auto* s = new HostMediaStreamAudioSourceNode();
    s->base.nodeType = AudioNodeType::MediaStreamSource;
    return g_mediaStreamAudioSourceNodeClass.make(s, hostMediaStreamAudioSourceNodeDtor);
}

// ---------------------------------------------------------------------------
// AudioContext Voice Methods
// ---------------------------------------------------------------------------

void registerAudioContextVoice(ObjectBuilder& b) {
    b.def("createVoice", 0, [](Value, std::span<const Value>) {
        auto* e = getAudioEngine();
        return ev::fromDouble(e ? e->createVoice() : -1);
    });

    b.def("removeVoice", 1, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) e->removeVoice(i32At(a, 0));
        return ev::undefined();
    });

    b.def("startVoice", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) {
            double when = a.size() >= 2 ? numAt(a, 1) : 0.0;
            e->startVoice(i32At(a, 0), when);
        }
        return ev::undefined();
    });

    b.def("stopVoice", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && !a.empty()) {
            double when = a.size() >= 2 ? numAt(a, 1) : 0.0;
            e->stopVoice(i32At(a, 0), when);
        }
        return ev::undefined();
    });

    b.def("setVoicePersistent", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoicePersistent(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceNote", 3, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) {
            float vel = a.size() >= 3 ? static_cast<float>(numAt(a, 2)) : 1.0f;
            e->setVoiceNote(i32At(a, 0), i32At(a, 1), vel);
        }
        return ev::undefined();
    });

    b.def("setVoiceWaveform", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) {
            e->setWaveform(i32At(a, 0), parseWaveform(ev::toUtf8(a[1])));
        }
        return ev::undefined();
    });

    b.def("setVoiceFrequency", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setFrequency(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceGain", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoicePan", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoicePan(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoicePitchBend", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoicePitchBend(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceAttackTime", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setAttackTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceDecayTime", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setDecayTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceSustainLevel", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setSustainLevel(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceReleaseTime", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setReleaseTime(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceFilterEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceFilterEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setVoiceFilterType", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceFilterType(i32At(a, 0), parseFilterType(ev::toUtf8(a[1])));
        return ev::undefined();
    });

    b.def("setVoiceFilterFrequency", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceFilterFrequency(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setVoiceFilterQ", 2, [](Value, std::span<const Value> a) {
        auto* e = getAudioEngine();
        if (e && a.size() >= 2) e->setVoiceFilterQ(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
}

// ---------------------------------------------------------------------------
// Globals Installation
// ---------------------------------------------------------------------------

void installAudioSynthGlobals() {
    g_voiceAllocatorClass.install("VoiceAllocator", 1,
        [](Value, std::span<const Value> a) {
            return makeVoiceAllocatorValue(a.empty() ? 16 : i32At(a, 0));
        },
        decorateVoiceAllocatorProto);

    g_modMatrixClass.install("ModMatrix", 0,
        [](Value, std::span<const Value>) { return makeModMatrixValue(); },
        decorateModMatrixProto);

    g_midiInputClass.install("MidiInput", 0,
        [](Value, std::span<const Value>) { return makeMidiInputValue(); },
        decorateMidiInputProto);

    g_mediaStreamClass.install("MediaStream", 0,
        [](Value, std::span<const Value>) { return makeMediaStreamValue(); },
        nullptr);

    g_mediaStreamAudioSourceNodeClass.install("MediaStreamAudioSourceNode", 0,
        [](Value, std::span<const Value>) { return makeMediaStreamAudioSourceNodeValue(); },
        decorateMediaStreamSourceNodeProto);
    g_mediaStreamAudioSourceNodeClass.inherit(g_audioNodeClass);
}

void installAudioSequencerGlobals() {
    g_sequenceClass.install("Sequence", 1,
        [](Value, std::span<const Value> a) {
            return a.empty() ? ev::null() : makeSequenceValue(a[0]);
        },
        decorateSequenceProto);
}

} // namespace broaudio::api
