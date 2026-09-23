#include "host_audio_internal.h"
#include <algorithm>
#include <cmath>

namespace broaudio::api {

HostClass g_audioParamClass;

void hostAudioParamDtor(void* p) {
    delete static_cast<HostAudioParamHandle*>(p);
}

static HostAudioParamHandle* paramHandleOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostAudioParamHandle*>(ev::handleData(v));
    if (!h || h->tag != kHostAudioParamTag) return nullptr;
    return h;
}

HostAudioParam* hostAudioParamOf(Value v) {
    HostAudioParamHandle* h = paramHandleOf(v);
    return h ? h->param.get() : nullptr;
}

ParamRef hostAudioParamRef(Value v) {
    HostAudioParamHandle* h = paramHandleOf(v);
    return h ? h->param : nullptr;
}

// syncAudioParamValue lives in host_audio_live.cpp with the rest of the
// param -> engine plumbing.

float HostAudioParam::evaluate(double t) const {
    if (timeline.empty()) {
        return std::clamp(value, minValue, maxValue);
    }

    if (t < timeline.front().time) {
        if (timeline.front().type == ParamEventType::LinearRamp ||
            timeline.front().type == ParamEventType::ExponentialRamp) {
            double t0 = 0.0;
            double t1 = timeline.front().time;
            float v0 = value;
            float v1 = timeline.front().value;
            if (t <= t0 || t1 <= t0) return std::clamp(v0, minValue, maxValue);
            float frac = static_cast<float>((t - t0) / (t1 - t0));
            if (timeline.front().type == ParamEventType::LinearRamp) {
                return std::clamp(v0 + (v1 - v0) * frac, minValue, maxValue);
            } else {
                if (v0 > 0.0f && v1 > 0.0f) {
                    return std::clamp(v0 * std::pow(v1 / v0, frac), minValue, maxValue);
                }
                return std::clamp(v0 + (v1 - v0) * frac, minValue, maxValue);
            }
        }
        return std::clamp(value, minValue, maxValue);
    }

    float curVal = value;
    double curTime = 0.0;

    for (size_t i = 0; i < timeline.size(); ++i) {
        const auto& ev = timeline[i];

        if (ev.time > t) {
            if (ev.type == ParamEventType::LinearRamp) {
                double t0 = curTime;
                double t1 = ev.time;
                float v0 = curVal;
                float v1 = ev.value;
                if (t1 <= t0) return std::clamp(v1, minValue, maxValue);
                float frac = static_cast<float>((t - t0) / (t1 - t0));
                return std::clamp(v0 + (v1 - v0) * frac, minValue, maxValue);
            } else if (ev.type == ParamEventType::ExponentialRamp) {
                double t0 = curTime;
                double t1 = ev.time;
                float v0 = curVal;
                float v1 = ev.value;
                if (t1 <= t0) return std::clamp(v1, minValue, maxValue);
                float frac = static_cast<float>((t - t0) / (t1 - t0));
                if (v0 > 0.0f && v1 > 0.0f) {
                    return std::clamp(v0 * std::pow(v1 / v0, frac), minValue, maxValue);
                }
                return std::clamp(v0 + (v1 - v0) * frac, minValue, maxValue);
            } else {
                break;
            }
        }

        switch (ev.type) {
        case ParamEventType::SetValue:
        case ParamEventType::LinearRamp:
        case ParamEventType::ExponentialRamp:
            curVal = ev.value;
            curTime = ev.time;
            break;
        case ParamEventType::SetTarget: {
            curTime = ev.time;
            float target = ev.value;
            float timeConst = ev.timeConstant;
            if (timeConst <= 0.0f) {
                curVal = target;
            } else {
                double evalT = t;
                if (i + 1 < timeline.size() && timeline[i + 1].time <= t) {
                    evalT = timeline[i + 1].time;
                }
                float v0 = curVal;
                curVal = target + (v0 - target) * std::exp(-static_cast<float>((evalT - curTime) / timeConst));
                curTime = evalT;
            }
            break;
        }
        case ParamEventType::SetValueCurve: {
            curTime = ev.time;
            if (ev.curve.empty()) break;
            double duration = ev.duration;
            if (t >= ev.time + duration || duration <= 0.0) {
                curVal = ev.curve.back();
                curTime = ev.time + duration;
            } else {
                double frac = (t - ev.time) / duration;
                double idx = frac * (ev.curve.size() - 1);
                size_t i0 = static_cast<size_t>(idx);
                size_t i1 = std::min(i0 + 1, ev.curve.size() - 1);
                float f = static_cast<float>(idx - i0);
                curVal = ev.curve[i0] + (ev.curve[i1] - ev.curve[i0]) * f;
                return std::clamp(curVal, minValue, maxValue);
            }
            break;
        }
        }
    }

    return std::clamp(curVal, minValue, maxValue);
}

void HostAudioParam::addSetValue(float val, double startTime) {
    ParamTimelineEvent ev;
    ev.type = ParamEventType::SetValue;
    ev.time = startTime;
    ev.value = std::clamp(val, minValue, maxValue);

    auto it = std::upper_bound(timeline.begin(), timeline.end(), startTime,
        [](double t, const ParamTimelineEvent& e) { return t < e.time; });
    timeline.insert(it, ev);
}

void HostAudioParam::addLinearRamp(float val, double endTime) {
    if (timeline.empty()) {
        auto* e = getAudioEngine();
        double curTime = e ? e->currentTime() : 0.0;
        addSetValue(value, curTime);
    }
    ParamTimelineEvent ev;
    ev.type = ParamEventType::LinearRamp;
    ev.time = endTime;
    ev.value = std::clamp(val, minValue, maxValue);

    auto it = std::upper_bound(timeline.begin(), timeline.end(), endTime,
        [](double t, const ParamTimelineEvent& e) { return t < e.time; });
    timeline.insert(it, ev);
}

void HostAudioParam::addExponentialRamp(float val, double endTime) {
    if (timeline.empty()) {
        auto* e = getAudioEngine();
        double curTime = e ? e->currentTime() : 0.0;
        addSetValue(value, curTime);
    }
    ParamTimelineEvent ev;
    ev.type = ParamEventType::ExponentialRamp;
    ev.time = endTime;
    ev.value = std::clamp(val, minValue, maxValue);

    auto it = std::upper_bound(timeline.begin(), timeline.end(), endTime,
        [](double t, const ParamTimelineEvent& e) { return t < e.time; });
    timeline.insert(it, ev);
}

void HostAudioParam::addSetTarget(float target, double startTime, float timeConstant) {
    if (timeline.empty()) {
        auto* e = getAudioEngine();
        double curTime = e ? e->currentTime() : 0.0;
        addSetValue(value, curTime);
    }
    ParamTimelineEvent ev;
    ev.type = ParamEventType::SetTarget;
    ev.time = startTime;
    ev.value = std::clamp(target, minValue, maxValue);
    ev.timeConstant = timeConstant;

    auto it = std::upper_bound(timeline.begin(), timeline.end(), startTime,
        [](double t, const ParamTimelineEvent& e) { return t < e.time; });
    timeline.insert(it, ev);
}

void HostAudioParam::addSetValueCurve(const float* data, size_t count, double startTime, double duration) {
    ParamTimelineEvent ev;
    ev.type = ParamEventType::SetValueCurve;
    ev.time = startTime;
    ev.duration = duration;
    if (data && count > 0) {
        ev.curve.assign(data, data + count);
        for (auto& v : ev.curve) {
            v = std::clamp(v, minValue, maxValue);
        }
    }
    auto it = std::upper_bound(timeline.begin(), timeline.end(), startTime,
        [](double t, const ParamTimelineEvent& e) { return t < e.time; });
    timeline.insert(it, ev);
}

void HostAudioParam::cancelScheduledValues(double cancelTime) {
    auto it = std::lower_bound(timeline.begin(), timeline.end(), cancelTime,
        [](const ParamTimelineEvent& e, double t) { return e.time < t; });
    timeline.erase(it, timeline.end());
}

void HostAudioParam::cancelAndHoldAtTime(double cancelTime) {
    float heldVal = evaluate(cancelTime);
    cancelScheduledValues(cancelTime);
    addSetValue(heldVal, cancelTime);
}

void decorateAudioParamProto(ObjectBuilder& b) {
    b.accessor("value",
               [](Value self_, std::span<const Value>) {
                   ParamRef p = hostAudioParamRef(self_);
                   if (!p) return ev::undefined();
                   if (p->timeline.empty()) return ev::fromDouble(p->value);
                   auto* e = getAudioEngine();
                   double t = e ? e->currentTime() : 0.0;
                   return ev::fromDouble(p->evaluate(t));
               },
               [](Value self_, std::span<const Value> a) {
                   ParamRef p = hostAudioParamRef(self_);
                   if (!p) return ev::undefined();
                   float v = static_cast<float>(numAt(a, 0));
                   p->timeline.clear();
                   syncAudioParamValue(p.get(), v);
                   return ev::undefined();
               });

    b.accessor("defaultValue", [](Value self_, std::span<const Value>) {
        ParamRef p = hostAudioParamRef(self_);
        return ev::fromDouble(p ? p->defaultValue : 1.0);
    }, nullptr);

    b.accessor("minValue", [](Value self_, std::span<const Value>) {
        ParamRef p = hostAudioParamRef(self_);
        return ev::fromDouble(p ? p->minValue : -3.4e38f);
    }, nullptr);

    b.accessor("maxValue", [](Value self_, std::span<const Value>) {
        ParamRef p = hostAudioParamRef(self_);
        return ev::fromDouble(p ? p->maxValue : 3.4e38f);
    }, nullptr);

    b.def("setValueAtTime", 2, [](Value self_, std::span<const Value> a) -> Value {
        ParamRef p = hostAudioParamRef(self_);
        if (p && !a.empty()) {
            float val = static_cast<float>(numAt(a, 0));
            double time = a.size() >= 2 ? numAt(a, 1) : 0.0;
            p->addSetValue(val, time);
            paramTimelineChanged(p);
        }
        return self_;
    });

    b.def("linearRampToValueAtTime", 2, [](Value self_, std::span<const Value> a) -> Value {
        ParamRef p = hostAudioParamRef(self_);
        if (p && !a.empty()) {
            float val = static_cast<float>(numAt(a, 0));
            double time = a.size() >= 2 ? numAt(a, 1) : 0.0;
            p->addLinearRamp(val, time);
            paramTimelineChanged(p);
        }
        return self_;
    });

    b.def("exponentialRampToValueAtTime", 2, [](Value self_, std::span<const Value> a) -> Value {
        ParamRef p = hostAudioParamRef(self_);
        if (p && !a.empty()) {
            float val = static_cast<float>(numAt(a, 0));
            double time = a.size() >= 2 ? numAt(a, 1) : 0.0;
            p->addExponentialRamp(val, time);
            paramTimelineChanged(p);
        }
        return self_;
    });

    b.def("setTargetAtTime", 3, [](Value self_, std::span<const Value> a) -> Value {
        ParamRef p = hostAudioParamRef(self_);
        if (p && !a.empty()) {
            float target = static_cast<float>(numAt(a, 0));
            double startTime = a.size() >= 2 ? numAt(a, 1) : 0.0;
            float timeConstant = a.size() >= 3 ? static_cast<float>(numAt(a, 2)) : 0.0f;
            p->addSetTarget(target, startTime, timeConstant);
            paramTimelineChanged(p);
        }
        return self_;
    });

    b.def("setValueCurveAtTime", 3, [](Value self_, std::span<const Value> a) -> Value {
        ParamRef p = hostAudioParamRef(self_);
        // Reading a plain-array curve allocates; the receiver is returned
        // afterwards, so it is rooted first.
        ev::Persistent self(self_);
        if (p && !a.empty()) {
            std::vector<float> storage;
            const float* data = nullptr;
            size_t count = 0;
            double startTime = a.size() >= 2 ? numAt(a, 1) : 0.0;
            double duration = a.size() >= 3 ? numAt(a, 2) : 0.0;
            if (floatData(a[0], storage, &data, &count) && count > 0) {
                p->addSetValueCurve(data, count, startTime, duration);
            }
            paramTimelineChanged(p);
        }
        return self.get();
    });

    b.def("cancelScheduledValues", 1, [](Value self_, std::span<const Value> a) -> Value {
        ParamRef p = hostAudioParamRef(self_);
        if (p && !a.empty()) {
            p->cancelScheduledValues(numAt(a, 0));
            paramTimelineChanged(p);
        }
        return self_;
    });

    b.def("cancelAndHoldAtTime", 1, [](Value self_, std::span<const Value> a) -> Value {
        ParamRef p = hostAudioParamRef(self_);
        if (p && !a.empty()) {
            p->cancelAndHoldAtTime(numAt(a, 0));
            paramTimelineChanged(p);
        }
        return self_;
    });

    b.def("getValueAtTime", 1, [](Value self_, std::span<const Value> a) -> Value {
        ParamRef p = hostAudioParamRef(self_);
        if (!p) return ev::undefined();
        double t = a.empty() ? 0.0 : numAt(a, 0);
        return ev::fromDouble(p->evaluate(t));
    });
}

ParamRef makeAudioParam(AudioParamTarget target, int targetId,
                        float initialVal, float minVal, float maxVal, float defaultVal) {
    auto param = std::make_shared<HostAudioParam>();
    param->target = target;
    param->targetId = targetId;
    param->value = initialVal;
    param->defaultValue = defaultVal;
    param->minValue = minVal;
    param->maxValue = maxVal;
    return param;
}

Value makeAudioParamValue(const ParamRef& param) {
    auto* h = new HostAudioParamHandle();
    h->param = param;
    return g_audioParamClass.make(h, hostAudioParamDtor);
}

Value makeAudioParamValue(AudioParamTarget target, int targetId,
                          float initialVal, float minVal, float maxVal, float defaultVal) {
    return makeAudioParamValue(makeAudioParam(target, targetId, initialVal, minVal, maxVal, defaultVal));
}

} // namespace broaudio::api
