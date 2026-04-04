#pragma once

namespace broaudio {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Listener position and orientation in 3D space.
// Used by the spatial mixer to compute per-source panning and attenuation.
struct Listener {
    Vec3 position;
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
};

} // namespace broaudio
