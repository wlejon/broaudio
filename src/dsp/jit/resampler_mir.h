#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if BROAUDIO_HAS_JIT
#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/target/msl_target.hpp>
#include <brass/target/ptx_target.hpp>
#include <brass/target/spirv_target.hpp>
#endif

namespace broaudio {

enum class MirResampleMode : uint8_t {
    Linear = 0,
    Cubic = 1,
    WindowedSinc = 2,
};

struct BatchDspMixParams {
    float step = 1.0f;          // inputRate / outputRate
    float gain = 1.0f;          // master output gain
    float panL = 1.0f;          // left channel pan multiplier
    float panR = 1.0f;          // right channel pan multiplier
    float softClipDrive = 1.0f; // pre-tanh drive (>= 0)
    float softClipMix = 0.0f;   // wet/dry soft-clipper mix in [0, 1]
};

// Polyphase Kaiser-windowed sinc constants (matching src/dsp/resampler.cpp).
inline constexpr int kMirSincHalfTaps = 16;
inline constexpr int kMirSincKernelSize = 2 * kMirSincHalfTaps;
inline constexpr int kMirSincNumPhases = 512;
inline constexpr double kMirSincKaiserBeta = 7.0;

// Builds the normalized polyphase filter table (NUM_PHASES * KERNEL_SIZE floats)
// for windowed-sinc resampling at the given input/output sample rates.
std::vector<float> buildMirSincFilterTable(int inputRate, int outputRate);

// Scalar C++ reference implementations (f32 precision, matching the single-source MIR).
std::vector<float> resampleMixReference(
    const float* inputInterleavedStereo,
    int inputFrames,
    int outputFrames,
    MirResampleMode mode,
    const float* filterTable,
    const BatchDspMixParams& params);

std::vector<float> stereoMixReference(
    const float* inputInterleavedStereo,
    int numFrames,
    const BatchDspMixParams& params);

#if BROAUDIO_HAS_JIT

// Function pointer signatures for CPU SIMD Brass JIT kernels.
using ResampleMixCpuFn = void (*)(
    const float* inBuf,
    const float* filterTable,
    float* outBuf,
    int32_t inFrames,
    int32_t outFrames,
    float step,
    float gain,
    float panL,
    float panR,
    float drive,
    float softClipMix);

using StereoMixCpuFn = void (*)(
    const float* inBuf,
    float* outBuf,
    int32_t numFrames,
    float gain,
    float panL,
    float panR,
    float drive,
    float softClipMix);

struct GpuTargetArtifacts {
    bool portable = false;
    std::string portabilityDiagnostics;
    brass::target::SpirvKernel spirv;
    std::string ptx;
    brass::target::MslKernel msl;
};

// Single-source MIR builders:
// - CPU SIMD kernels loop over frames [0, outFrames) and compile via Brass KernelJit
//   with AVX2/SIMD vectorization enabled.
// - GPU kernels dispatch over gpu_global_tid_x with bounds guard, use pure f32/i32,
//   and pass brass::verify_gpu_portability().
brass::Function* buildResampleMixCpuMir(MirResampleMode mode, brass::Module& mod);
brass::Function* buildResampleMixGpuMir(MirResampleMode mode, brass::Module& mod);

brass::Function* buildStereoMixCpuMir(brass::Module& mod);
brass::Function* buildStereoMixGpuMir(brass::Module& mod);

// Compile CPU SIMD kernels via Brass KernelJit (cached in audio_native_module_cache).
std::shared_ptr<brass::codegen::KernelFunction> compileResampleMixCpuKernel(MirResampleMode mode);
std::shared_ptr<brass::codegen::KernelFunction> compileStereoMixCpuKernel();

// Verify GPU portability and compile single-source GPU MIR to SPIR-V, PTX, and MSL.
GpuTargetArtifacts compileResampleMixGpuTargets(MirResampleMode mode, uint32_t localSizeX = 256);
GpuTargetArtifacts compileStereoMixGpuTargets(uint32_t localSizeX = 256);

// Execute a compiled SPIR-V resampler+mix kernel on Vulkan (requires brass::gpu::vulkan_available()).
bool runResampleMixVulkan(
    const brass::target::SpirvKernel& kernel,
    const float* inputInterleavedStereo,
    int inputFrames,
    int outputFrames,
    const float* filterTable,
    size_t filterTableSize,
    const BatchDspMixParams& params,
    std::vector<float>& outInterleavedStereo,
    std::string* outError = nullptr);

// Execute a compiled SPIR-V stereo gain/pan/soft-clipper mix kernel on Vulkan.
bool runStereoMixVulkan(
    const brass::target::SpirvKernel& kernel,
    const float* inputInterleavedStereo,
    int numFrames,
    const BatchDspMixParams& params,
    std::vector<float>& outInterleavedStereo,
    std::string* outError = nullptr);

#endif // BROAUDIO_HAS_JIT

} // namespace broaudio
