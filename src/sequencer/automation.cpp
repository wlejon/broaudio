#include "broaudio/sequencer/automation.h"

#include <algorithm>
#include <cmath>

namespace broaudio {

AutomationLane::AutomationLane(ApplyFn applyFn)
    : applyFn_(std::move(applyFn))
{
}

void AutomationLane::addPoint(double beat, float value)
{
    points_.push_back({beat, value});
    sortPoints();
}

void AutomationLane::removePoint(int index)
{
    if (index >= 0 && index < static_cast<int>(points_.size())) {
        points_.erase(points_.begin() + index);
    }
}

void AutomationLane::setPointValue(int index, float value)
{
    if (index >= 0 && index < static_cast<int>(points_.size())) {
        points_[index].value = value;
    }
}

void AutomationLane::clearPoints()
{
    points_.clear();
}

float AutomationLane::evaluate(double beat) const
{
    if (points_.empty()) return 0.0f;
    if (points_.size() == 1) return points_[0].value;

    // Before first point: clamp
    if (beat <= points_.front().beat) return points_.front().value;
    // After last point: clamp
    if (beat >= points_.back().beat) return points_.back().value;

    // Binary search for the segment containing beat
    // Find first point with beat > target
    auto it = std::upper_bound(points_.begin(), points_.end(), beat,
        [](double b, const AutomationPoint& p) { return b < p.beat; });

    const auto& p1 = *(it - 1);
    const auto& p2 = *it;

    switch (mode_) {
        case InterpMode::Step:
            return p1.value;

        case InterpMode::Linear: {
            double t = (beat - p1.beat) / (p2.beat - p1.beat);
            return p1.value + static_cast<float>(t) * (p2.value - p1.value);
        }

        case InterpMode::Smooth: {
            double t = (beat - p1.beat) / (p2.beat - p1.beat);
            // Cosine interpolation: smooth ease-in/ease-out, no overshoot
            float ct = static_cast<float>((1.0 - std::cos(t * 3.14159265358979323846)) * 0.5);
            return p1.value + ct * (p2.value - p1.value);
        }
    }

    return p1.value;
}

void AutomationLane::apply(double beat)
{
    if (points_.empty()) return;
    applyFn_(evaluate(beat));
}

void AutomationLane::sortPoints()
{
    std::sort(points_.begin(), points_.end(),
              [](const AutomationPoint& a, const AutomationPoint& b) {
                  return a.beat < b.beat;
              });
}

} // namespace broaudio
