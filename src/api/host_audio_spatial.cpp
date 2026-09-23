#include "host_audio_internal.h"

namespace broaudio::api {

HostClass g_pannerNodeClass;
HostClass g_stereoPannerNodeClass;

void hostPannerDtor(void* p) {
    delete static_cast<HostPannerNode*>(p);
}

void hostStereoPannerDtor(void* p) {
    delete static_cast<HostStereoPannerNode*>(p);
}

void decoratePannerNodeProto(ObjectBuilder& b) {
    b.accessor("panningModel",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromUtf8(p->panningModel);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p || a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string m = ev::toUtf8(a[0]);
                   if (m == "equalpower" || m == "HRTF") {
                       p->panningModel = m;
                   }
                   return ev::undefined();
               });

    b.accessor("distanceModel",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromUtf8(p->distanceModel);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p || a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string m = ev::toUtf8(a[0]);
                   if (m == "inverse" || m == "linear" || m == "exponential") {
                       p->distanceModel = m;
                   }
                   return ev::undefined();
               });

    b.accessor("refDistance",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->refDistance);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->refDistance = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("maxDistance",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->maxDistance);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->maxDistance = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("rolloffFactor",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->rolloffFactor);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->rolloffFactor = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("coneInnerAngle",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->coneInnerAngle);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->coneInnerAngle = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("coneOuterAngle",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->coneOuterAngle);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->coneOuterAngle = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("coneOuterGain",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->coneOuterGain);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->coneOuterGain = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.def("setPosition", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostPannerNode* p = pannerOf(self_);
        if (p && a.size() >= 3) {
            p->posX = static_cast<float>(numAt(a, 0));
            p->posY = static_cast<float>(numAt(a, 1));
            p->posZ = static_cast<float>(numAt(a, 2));
            const float xyz[3] = {p->posX, p->posY, p->posZ};
            for (int i = 0; i < 3; ++i) {
                p->positionParams[i]->timeline.clear();
                syncAudioParamValue(p->positionParams[i].get(), xyz[i]);
            }
        }
        return ev::undefined();
    });

    b.def("setOrientation", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostPannerNode* p = pannerOf(self_);
        if (p && a.size() >= 3) {
            p->orientX = static_cast<float>(numAt(a, 0));
            p->orientY = static_cast<float>(numAt(a, 1));
            p->orientZ = static_cast<float>(numAt(a, 2));
            const float xyz[3] = {p->orientX, p->orientY, p->orientZ};
            for (int i = 0; i < 3; ++i) {
                p->orientationParams[i]->timeline.clear();
                syncAudioParamValue(p->orientationParams[i].get(), xyz[i]);
            }
        }
        return ev::undefined();
    });
}

void syncPannerFromParams(Value pannerObj, double when) {
    HostPannerNode* p = pannerOf(pannerObj);
    if (!p) return;
    p->posX = p->positionParams[0]->evaluate(when);
    p->posY = p->positionParams[1]->evaluate(when);
    p->posZ = p->positionParams[2]->evaluate(when);
    p->orientX = p->orientationParams[0]->evaluate(when);
    p->orientY = p->orientationParams[1]->evaluate(when);
    p->orientZ = p->orientationParams[2]->evaluate(when);
}

void pushConnectedTargets(const std::vector<ev::Persistent>& targets,
                          std::vector<HostAudioNode*>& queue, double when) {
    for (const auto& t : targets) {
        HostAudioNode* n = hostAudioNodeOf(t.get());
        if (!n) continue;
        if (n->nodeType == AudioNodeType::Panner) syncPannerFromParams(t.get(), when);
        queue.push_back(n);
    }
}

Value makePannerNodeValue() {
    auto* panner = new HostPannerNode();
    panner->base.nodeType = AudioNodeType::Panner;

    panner->positionParams[0] = makeAudioParam(AudioParamTarget::PannerPositionX, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f);
    panner->positionParams[1] = makeAudioParam(AudioParamTarget::PannerPositionY, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f);
    panner->positionParams[2] = makeAudioParam(AudioParamTarget::PannerPositionZ, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f);
    panner->orientationParams[0] = makeAudioParam(AudioParamTarget::PannerOrientationX, -1, 1.0f, -3.4e38f, 3.4e38f, 1.0f);
    panner->orientationParams[1] = makeAudioParam(AudioParamTarget::PannerOrientationY, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f);
    panner->orientationParams[2] = makeAudioParam(AudioParamTarget::PannerOrientationZ, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f);

    ObjectBuilder b(g_pannerNodeClass.make(panner, hostPannerDtor));
    b.set("positionX", makeAudioParamValue(panner->positionParams[0]));
    b.set("positionY", makeAudioParamValue(panner->positionParams[1]));
    b.set("positionZ", makeAudioParamValue(panner->positionParams[2]));
    b.set("orientationX", makeAudioParamValue(panner->orientationParams[0]));
    b.set("orientationY", makeAudioParamValue(panner->orientationParams[1]));
    b.set("orientationZ", makeAudioParamValue(panner->orientationParams[2]));
    return b.get();
}

void decorateStereoPannerNodeProto(ObjectBuilder& b) {
    b.accessor("pan",
               [](Value self_, std::span<const Value>) {
                   HostStereoPannerNode* p = stereoPannerOf(self_);
                   if (!p) return ev::undefined();
                   // The AudioParam lives on the instance as `_pan` (and as
                   // an own `pan`, which shadows this accessor).
                   return ev::getProperty(self_, "_pan");
               },
               [](Value self_, std::span<const Value> a) {
                   HostStereoPannerNode* p = stereoPannerOf(self_);
                   if (!p || a.empty()) return ev::undefined();
                   float val = static_cast<float>(numAt(a, 0));
                   p->pan = val;
                   p->panParam->timeline.clear();
                   syncAudioParamValue(p->panParam.get(), val);
                   return ev::undefined();
               });
}

Value makeStereoPannerNodeValue() {
    auto* panner = new HostStereoPannerNode();
    panner->base.nodeType = AudioNodeType::StereoPanner;

    panner->panParam = makeAudioParam(AudioParamTarget::Pan, -1, 0.0f, -1.0f, 1.0f, 0.0f);

    ObjectBuilder b(g_stereoPannerNodeClass.make(panner, hostStereoPannerDtor));
    ev::Persistent panVal(makeAudioParamValue(panner->panParam));
    b.set("_pan", panVal.get());
    b.set("pan", panVal.get());
    return b.get();
}

// AudioDestinationNode.prototype: maxChannelCount lives here, not on the
// instance, so `ctx.destination` is an ordinary AudioDestinationNode whose
// chain is AudioDestinationNode.prototype -> AudioNode.prototype (connect and
// disconnect stay the single base methods the class check pins).
void decorateAudioDestinationNodeProto(ObjectBuilder& b) {
    b.accessor("maxChannelCount", [](Value, std::span<const Value>) {
        return ev::fromDouble(2.0);
    }, nullptr);
}

Value makeDestinationNodeValue() {
    auto* dest = new HostAudioNode();
    dest->nodeType = AudioNodeType::Destination;
    return g_audioDestinationNodeClass.make(dest, hostAudioNodeDtor);
}

Value makeListenerValue() {
    ObjectBuilder b;

    b.def("setPosition", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) {
            e->setListenerPosition(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setOrientation", 6, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (e && a.size() >= 6) {
            e->setListenerOrientation(static_cast<float>(numAt(a, 0)),
                                      static_cast<float>(numAt(a, 1)),
                                      static_cast<float>(numAt(a, 2)),
                                      static_cast<float>(numAt(a, 3)),
                                      static_cast<float>(numAt(a, 4)),
                                      static_cast<float>(numAt(a, 5)));
        }
        return ev::undefined();
    });

    b.def("setVelocity", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) {
            e->setListenerVelocity(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setListenerPosition", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (e && a.size() >= 3) {
            e->setListenerPosition(static_cast<float>(numAt(a, 0)),
                                   static_cast<float>(numAt(a, 1)),
                                   static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("setListenerOrientation", 6, [](Value, std::span<const Value> a) -> Value {
        auto* e = getAudioEngine();
        if (e && a.size() >= 6) {
            e->setListenerOrientation(static_cast<float>(numAt(a, 0)),
                                      static_cast<float>(numAt(a, 1)),
                                      static_cast<float>(numAt(a, 2)),
                                      static_cast<float>(numAt(a, 3)),
                                      static_cast<float>(numAt(a, 4)),
                                      static_cast<float>(numAt(a, 5)));
        }
        return ev::undefined();
    });

    b.set("positionX", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("positionY", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("positionZ", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("forwardX", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -1.0f, 1.0f, 0.0f));
    b.set("forwardY", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -1.0f, 1.0f, 0.0f));
    b.set("forwardZ", makeAudioParamValue(AudioParamTarget::Generic, -1, -1.0f, -1.0f, 1.0f, -1.0f));
    b.set("upX", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -1.0f, 1.0f, 0.0f));
    b.set("upY", makeAudioParamValue(AudioParamTarget::Generic, -1, 1.0f, -1.0f, 1.0f, 1.0f));
    b.set("upZ", makeAudioParamValue(AudioParamTarget::Generic, -1, 0.0f, -1.0f, 1.0f, 0.0f));

    return b.get();
}

} // namespace broaudio::api
