#pragma once

#include "broaudio/dsp/jit/jit_types.h"
#include <memory>

namespace brass {
class Module;
class Function;
}

namespace brass::codegen {
class KernelFunction;
}

namespace broaudio {

// Emits the unoptimized MIR function for a fused bus DSP chain topology into `mod`.
brass::Function* buildBusKernelMir(const JitTopology& topology, brass::Module& mod);

// Compiles a fused bus DSP chain topology into a native machine code kernel via brass::AudioKernelBuilder
std::shared_ptr<brass::codegen::KernelFunction> buildBusKernel(const JitTopology& topology);

} // namespace broaudio
