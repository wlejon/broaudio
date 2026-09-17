#pragma once

#include "broaudio/types.h"
#include "broaudio/dsp/params.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace broaudio {

// POD coefficient set for Direct Form II Transposed biquad:
//   y   = b0 * x + z1
//   z1' = b1 * x - a1 * y + z2
//   z2' = b2 * x - a2 * y
struct BiquadCoeffsPod {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

// POD register state for Direct Form II Transposed biquad
struct BiquadStatePod {
    float z1 = 0.0f;
    float z2 = 0.0f;
};

// Structural descriptor of a JIT-compiled bus DSP chain.
// Two buses with identical topologies share the same JIT-compiled machine code;
// individual coefficients, drives, and gains are passed in BusJitParams at runtime.
struct JitTopology {
    static constexpr int MAX_FILTERS = 4;

    int filterCount = 0; // Number of active filters (0..MAX_FILTERS)
    DistortionMode distortionMode = DistortionMode::SoftClip;
    bool hasDistortion = false;
    bool fuseGainPan = false;

    bool operator==(const JitTopology& o) const noexcept {
        return filterCount == o.filterCount &&
               distortionMode == o.distortionMode &&
               hasDistortion == o.hasDistortion &&
               fuseGainPan == o.fuseGainPan;
    }

    bool operator!=(const JitTopology& o) const noexcept {
        return !(*this == o);
    }

    size_t hash() const noexcept {
        size_t h = std::hash<int>{}(filterCount);
        h ^= (std::hash<int>{}(static_cast<int>(distortionMode)) << 2);
        h ^= (std::hash<bool>{}(hasDistortion) << 5);
        h ^= (std::hash<bool>{}(fuseGainPan) << 6);
        return h;
    }
};

struct JitTopologyHash {
    size_t operator()(const JitTopology& t) const noexcept {
        return t.hash();
    }
};

// Runtime parameters passed from host to the compiled JIT kernel.
// Main thread / engine updates these per audio block; kernel reads with zero locks.
struct alignas(16) BusJitParams {
    int activeFilterCount = 0;
    float distortionDrive = 1.0f;
    float distortionMix = 0.0f;
    float distortionOutputGain = 1.0f;
    float gainStart = 1.0f;
    float gainStep = 0.0f;
    float panLStart = 1.0f;
    float panLStep = 0.0f;
    float panRStart = 1.0f;
    float panRStep = 0.0f;
    BiquadCoeffsPod filters[4];
};

// Filter register states across audio block boundaries (audio thread only).
struct alignas(16) BusJitState {
    BiquadStatePod filterStateL[4];
    BiquadStatePod filterStateR[4];

    void reset() noexcept {
        for (int i = 0; i < 4; i++) {
            filterStateL[i] = BiquadStatePod{};
            filterStateR[i] = BiquadStatePod{};
        }
    }
};

// Function pointer signature emitted by brass for fused bus DSP
using BusJitFn = void (*)(float* buffer, int numFrames, const BusJitParams* params, BusJitState* state);

} // namespace broaudio
