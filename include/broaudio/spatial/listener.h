#pragma once

#include "broaudio/types.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace broaudio {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
};

// Per-source spatial parameters. Voices and clips each own one of these.
// All fields are atomic for lock-free main -> audio thread access.
struct SpatialSource {
    std::atomic<bool> spatialEnabled{false};
    std::atomic<float> posX{0.0f}, posY{0.0f}, posZ{0.0f};
    std::atomic<float> refDistance{1.0f};    // distance at which gain = 1.0
    std::atomic<float> maxDistance{100.0f};
    std::atomic<float> rolloff{1.0f};
    std::atomic<int> distanceModel{static_cast<int>(DistanceModel::Inverse)};

    Vec3 position() const {
        return {posX.load(std::memory_order_relaxed),
                posY.load(std::memory_order_relaxed),
                posZ.load(std::memory_order_relaxed)};
    }
};

// Listener position and orientation in 3D space.
// Used by the spatial mixer to compute per-source panning and attenuation.
// All fields are atomic for lock-free main→audio thread access.
struct Listener {
    std::atomic<float> posX{0.0f}, posY{0.0f}, posZ{0.0f};
    std::atomic<float> fwdX{0.0f}, fwdY{0.0f}, fwdZ{-1.0f};
    std::atomic<float> upX{0.0f},  upY{1.0f},  upZ{0.0f};

    Vec3 position() const {
        return {posX.load(std::memory_order_relaxed),
                posY.load(std::memory_order_relaxed),
                posZ.load(std::memory_order_relaxed)};
    }
    Vec3 forward() const {
        return {fwdX.load(std::memory_order_relaxed),
                fwdY.load(std::memory_order_relaxed),
                fwdZ.load(std::memory_order_relaxed)};
    }
    Vec3 up() const {
        return {upX.load(std::memory_order_relaxed),
                upY.load(std::memory_order_relaxed),
                upZ.load(std::memory_order_relaxed)};
    }
};

// Compute gain attenuation and stereo pan from a spatial source relative to a listener.
// Returns attenuation (0-1) and writes pan (-1..1) to outPan.
inline float computeSpatial(const Listener& listener, const SpatialSource& src, float& outPan)
{
    Vec3 lPos = listener.position();
    Vec3 sPos = src.position();
    Vec3 dir = sPos - lPos;
    float dist = dir.length();

    // Distance attenuation
    float refDist = src.refDistance.load(std::memory_order_relaxed);
    float maxDist = src.maxDistance.load(std::memory_order_relaxed);
    float rolloff = src.rolloff.load(std::memory_order_relaxed);
    auto model = static_cast<DistanceModel>(src.distanceModel.load(std::memory_order_relaxed));

    float gain = 1.0f;
    float d = std::max(dist, refDist);
    d = std::min(d, maxDist);

    switch (model) {
        case DistanceModel::Linear:
            if (maxDist > refDist)
                gain = 1.0f - rolloff * (d - refDist) / (maxDist - refDist);
            break;
        case DistanceModel::Inverse:
            gain = refDist / (refDist + rolloff * (d - refDist));
            break;
        case DistanceModel::Exponential:
            if (refDist > 0.0f)
                gain = std::pow(d / refDist, -rolloff);
            break;
    }
    gain = std::max(0.0f, std::min(1.0f, gain));

    // Angle-based panning (project source direction onto listener's right vector)
    outPan = 0.0f;
    if (dist > 0.0001f) {
        Vec3 fwd = listener.forward();
        Vec3 up = listener.up();
        Vec3 right = fwd.cross(up);
        float rightLen = right.length();
        if (rightLen > 0.0001f) {
            // Normalize
            right.x /= rightLen; right.y /= rightLen; right.z /= rightLen;
            Vec3 normDir = {dir.x / dist, dir.y / dist, dir.z / dist};
            outPan = std::clamp(normDir.dot(right), -1.0f, 1.0f);
        }
    }

    return gain;
}

} // namespace broaudio
