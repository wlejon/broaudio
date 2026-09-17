#pragma once

#include "broaudio/dsp/jit/jit_types.h"
#include <memory>

namespace brass::codegen {
class KernelFunction;
}

namespace broaudio {

// Compiles a fused bus DSP chain topology into a native machine code kernel via brass::AudioKernelBuilder
std::shared_ptr<brass::codegen::KernelFunction> buildBusKernel(const JitTopology& topology);

} // namespace broaudio
