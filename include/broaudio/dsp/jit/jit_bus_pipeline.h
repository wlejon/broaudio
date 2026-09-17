#pragma once

#include "broaudio/dsp/jit/jit_types.h"
#include <memory>

namespace brass::codegen {
class KernelFunction;
}

namespace broaudio {

struct Bus;

// An instantiated, ready-to-execute JIT bus DSP pipeline.
// Owned by Bus via AtomicSharedPtr; read by audio callback with zero locks.
class JitBusPipeline {
public:
    JitBusPipeline(JitTopology topology,
                   std::shared_ptr<brass::codegen::KernelFunction> kernel,
                   BusJitFn fnPtr) noexcept;
    ~JitBusPipeline() = default;

    JitBusPipeline(const JitBusPipeline&) = delete;
    JitBusPipeline& operator=(const JitBusPipeline&) = delete;

    const JitTopology& topology() const noexcept { return topology_; }

    // Check if the current configuration of the bus matches this pipeline's topology
    bool matches(const Bus& bus) const noexcept;

    // Reset filter states (z1, z2) across all channels
    void resetState() noexcept { state_.reset(); }

    // Update coefficients and parameter ramps for the current audio block
    void updateParams(Bus& bus, int numFrames, int sampleRate);

    // Execute the fused JIT kernel on the interleaved stereo buffer
    void process(float* buffer, int numFrames) noexcept {
        if (fnPtr_) {
            fnPtr_(buffer, numFrames, &params_, &state_);
        }
    }

    BusJitFn functionPointer() const noexcept { return fnPtr_; }
    const BusJitState& state() const noexcept { return state_; }
    BusJitState& state() noexcept { return state_; }
    const BusJitParams& params() const noexcept { return params_; }
    BusJitParams& params() noexcept { return params_; }

private:
    JitTopology topology_;
    std::shared_ptr<brass::codegen::KernelFunction> kernel_;
    BusJitFn fnPtr_ = nullptr;
    BusJitParams params_{};
    BusJitState state_{};
};

} // namespace broaudio
