#pragma once

#include "broaudio/spatial/listener.h"
#include "broaudio/types.h"
#include <atomic>

namespace broaudio {

// A spatialized audio source. The engine computes gain and pan
// from the source position relative to the listener.
struct SpatialSource {
    int id = 0;
    std::atomic<float> x{0.0f};
    std::atomic<float> y{0.0f};
    std::atomic<float> z{0.0f};

    // Distance attenuation parameters
    DistanceModel distanceModel = DistanceModel::Inverse;
    float refDistance = 1.0f;    // distance at which gain = 1
    float maxDistance = 100.0f;
    float rolloffFactor = 1.0f;

    // Cone (optional directional attenuation)
    float coneInnerAngle = 360.0f;  // full omnidirectional by default
    float coneOuterAngle = 360.0f;
    float coneOuterGain = 0.0f;
};

// Compute distance attenuation for a source relative to a listener.
float computeDistanceGain(const SpatialSource& source, const Listener& listener);

// Compute stereo pan (-1..1) from source position relative to listener.
float computeSpatialPan(const SpatialSource& source, const Listener& listener);

} // namespace broaudio
