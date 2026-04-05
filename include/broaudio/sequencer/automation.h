#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace broaudio {

enum class InterpMode : uint8_t { Step, Linear, Smooth };

struct AutomationPoint {
    double beat;    // beat position
    float value;    // parameter value at this point
};

class AutomationLane {
public:
    using ApplyFn = std::function<void(float value)>;

    explicit AutomationLane(ApplyFn applyFn);

    // Interpolation mode
    void setInterpMode(InterpMode mode) { mode_ = mode; }
    InterpMode interpMode() const { return mode_; }

    // Breakpoint management (keeps points sorted by beat)
    void addPoint(double beat, float value);
    void removePoint(int index);
    void setPointValue(int index, float value);
    void clearPoints();
    int pointCount() const { return static_cast<int>(points_.size()); }
    const AutomationPoint& point(int index) const { return points_[index]; }

    // Evaluate interpolated value at a given beat
    float evaluate(double beat) const;

    // Evaluate and call the apply function
    void apply(double beat);

private:
    void sortPoints();

    ApplyFn applyFn_;
    InterpMode mode_ = InterpMode::Linear;
    std::vector<AutomationPoint> points_;
};

} // namespace broaudio
