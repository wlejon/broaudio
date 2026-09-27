#pragma once

#include "broaudio/spatial/voice_jit.h"
#include <memory>

namespace brass::codegen {
class KernelFunction;
}

namespace broaudio {

// The air lane kernel's per-section block: kAirLanes lanes of each of the
// section's pole, per-sample pole step, mix, mix step, lowpass output and
// previous input. The kernel reads all six and writes z and xp back.
struct AirLaneSection {
    float p[kAirLanes];
    float dp[kAirLanes];
    float m[kAirLanes];
    float dm[kAirLanes];
    float z[kAirLanes];
    float xp[kAirLanes];
};

// Emits and compiles the chain kernel for one shape (a VoiceJitFn; the
// topology has no air stage). Throws on a brass failure; null when the build
// has no brass backend.
std::shared_ptr<brass::codegen::KernelFunction> buildVoiceKernel(const VoiceJitTopology& topology);

// Emits and compiles the air lane kernel (an AirJitFn). Same failure rules.
std::shared_ptr<brass::codegen::KernelFunction> buildAirLaneKernel();

} // namespace broaudio
