#pragma once

#include <atomic>

namespace broaudio {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
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

} // namespace broaudio
