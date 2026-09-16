// Sequence automation beyond addAutomationLane (host_audio_synth.cpp):
// lane removal and count, the points on a lane, and per-lane interpolation.
// Decorates the Sequence prototype that decorateSequenceProto builds.

#include "host_audio_internal.h"

#include <broaudio/sequencer/automation.h>

namespace broaudio::api {

namespace {

broaudio::InterpMode parseInterpMode(const std::string& str) {
    if (str == "step") return broaudio::InterpMode::Step;
    if (str == "smooth") return broaudio::InterpMode::Smooth;
    return broaudio::InterpMode::Linear;
}

const char* interpModeToString(broaudio::InterpMode mode) {
    switch (mode) {
        case broaudio::InterpMode::Step: return "step";
        case broaudio::InterpMode::Smooth: return "smooth";
        default: return "linear";
    }
}

bool laneInRange(HostSequence* h, int lane) {
    return h && h->seq && lane >= 0 && lane < h->seq->automationLaneCount();
}

} // namespace

void decorateSequenceAutomation(ObjectBuilder& b) {
    b.def("removeAutomationLane", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (!a.empty() && laneInRange(h, i32At(a, 0))) {
            int idx = i32At(a, 0);
            h->seq->removeAutomationLane(idx);
            if (idx < static_cast<int>(h->automationCallbacks.size())) {
                h->automationCallbacks.erase(h->automationCallbacks.begin() + idx);
            }
        }
        return ev::undefined();
    });

    b.def("clearAutomationLanes", 0, [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            h->seq->clearAutomationLanes();
            h->automationCallbacks.clear();
        }
        return ev::undefined();
    });

    b.accessor("automationLaneCount", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromDouble(h && h->seq ? h->seq->automationLaneCount() : 0);
    }, nullptr);

    b.def("addAutomationPoint", 3, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (a.size() >= 3 && laneInRange(h, i32At(a, 0))) {
            h->seq->automationLane(i32At(a, 0)).addPoint(numAt(a, 1), static_cast<float>(numAt(a, 2)));
        }
        return ev::undefined();
    });

    b.def("removeAutomationPoint", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (a.size() >= 2 && laneInRange(h, i32At(a, 0))) {
            h->seq->automationLane(i32At(a, 0)).removePoint(i32At(a, 1));
        }
        return ev::undefined();
    });

    b.def("clearAutomationPoints", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (!a.empty() && laneInRange(h, i32At(a, 0))) {
            h->seq->automationLane(i32At(a, 0)).clearPoints();
        }
        return ev::undefined();
    });

    b.def("setAutomationInterpMode", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (a.size() >= 2 && laneInRange(h, i32At(a, 0))) {
            h->seq->automationLane(i32At(a, 0)).setInterpMode(parseInterpMode(ev::toUtf8(a[1])));
        }
        return ev::undefined();
    });

    b.def("automationPointCount", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (!a.empty() && laneInRange(h, i32At(a, 0))) {
            return ev::fromDouble(h->seq->automationLane(i32At(a, 0)).pointCount());
        }
        return ev::fromDouble(0);
    });

    b.def("automationPoint", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = hostSequenceOf(self);
        if (a.size() < 2 || !laneInRange(h, i32At(a, 0))) return ev::null();
        int lane = i32At(a, 0);
        int ptIdx = i32At(a, 1);
        if (ptIdx < 0 || ptIdx >= h->seq->automationLane(lane).pointCount()) return ev::null();
        const auto& pt = h->seq->automationLane(lane).point(ptIdx);
        ObjectBuilder obj;
        obj.set("beat", ev::fromDouble(pt.beat));
        obj.set("value", ev::fromDouble(pt.value));
        return obj.get();
    });

    b.def("automationInterpMode", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (!a.empty() && laneInRange(h, i32At(a, 0))) {
            return ev::fromUtf8(interpModeToString(h->seq->automationLane(i32At(a, 0)).interpMode()));
        }
        return ev::fromUtf8("linear");
    });
}

} // namespace broaudio::api
