#include "test_harness.h"
#include "dsp/jit/resampler_mir.h"
#include "broaudio/dsp/resampler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

#if BROAUDIO_HAS_JIT
#include <brass/gpu/vulkan_driver.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/gpu_portability.hpp>
#endif

using namespace broaudio;

#if BROAUDIO_HAS_JIT

namespace {

std::vector<float> makeStereoTestSignal(int numFrames, int sampleRate)
{
    std::vector<float> buf(static_cast<size_t>(numFrames) * 2);
    for (int i = 0; i < numFrames; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        buf[i * 2 + 0] = 0.65f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * t) +
                         0.25f * std::sin(2.0f * std::numbers::pi_v<float> * 1760.0f * t);
        buf[i * 2 + 1] = 0.55f * std::cos(2.0f * std::numbers::pi_v<float> * 660.0f * t) +
                         0.20f * std::sin(2.0f * std::numbers::pi_v<float> * 2640.0f * t);
    }
    return buf;
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    float worst = 0.0f;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

void verifyGpuArtifactsValid(const GpuTargetArtifacts& art, const char* label)
{
    if (!art.portable) {
        std::printf("  [%s] portability failure:\n%s\n", label, art.portabilityDiagnostics.c_str());
    }
    ASSERT_TRUE(art.portable);
    ASSERT_TRUE(art.spirv.words.size() > 5);
    ASSERT_EQ(art.spirv.words[0], 0x07230203u); // SPIR-V magic word
    ASSERT_TRUE(art.spirv.push_constant_bytes <= 128u);
    ASSERT_TRUE(!art.ptx.empty());
    ASSERT_TRUE(art.ptx.find(".entry") != std::string::npos);
    ASSERT_TRUE(!art.msl.source.empty());
    ASSERT_TRUE(art.msl.source.find("kernel void") != std::string::npos);
}

void runModeParityAndGpuTest(MirResampleMode mode, const char* label, int inRate, int outRate, int inFrames)
{
    auto input = makeStereoTestSignal(inFrames, inRate);
    int outFrames = static_cast<int>(std::ceil(static_cast<double>(inFrames) * outRate / inRate));

    std::vector<float> filterTable;
    if (mode == MirResampleMode::WindowedSinc) {
        filterTable = buildMirSincFilterTable(inRate, outRate);
        ASSERT_EQ(filterTable.size(), static_cast<size_t>(kMirSincNumPhases * kMirSincKernelSize));
    }

    BatchDspMixParams params;
    params.step = static_cast<float>(static_cast<double>(inRate) / static_cast<double>(outRate));
    params.gain = 0.85f;
    params.panL = 0.78f;
    params.panR = 0.62f;
    params.softClipDrive = 2.25f;
    params.softClipMix = 0.70f;

    // 1. CPU Brass MIR SIMD kernel vs scalar C++ reference
    auto cpuKernel = compileResampleMixCpuKernel(mode);
    ASSERT_TRUE(cpuKernel != nullptr && cpuKernel->is_valid());
    auto cpuFn = cpuKernel->as<ResampleMixCpuFn>();
    ASSERT_TRUE(cpuFn != nullptr);

    std::vector<float> cpuOut(static_cast<size_t>(outFrames) * 2, 0.0f);
    cpuFn(
        input.data(),
        filterTable.empty() ? nullptr : filterTable.data(),
        cpuOut.data(),
        inFrames,
        outFrames,
        params.step,
        params.gain,
        params.panL,
        params.panR,
        params.softClipDrive,
        params.softClipMix);

    std::vector<float> refOut = resampleMixReference(
        input.data(), inFrames, outFrames, mode,
        filterTable.empty() ? nullptr : filterTable.data(), params);
    ASSERT_EQ(cpuOut.size(), refOut.size());

    float cpuRefDiff = maxAbsDiff(cpuOut, refOut);
    std::printf("  [cpu-mir] %-14s max abs diff vs scalar ref = %.3g\n",
                label, static_cast<double>(cpuRefDiff));
    ASSERT_LT(cpuRefDiff, 1e-5f);

    // 2. Single-source GPU MIR portability + SPIR-V / PTX / MSL codegen
    GpuTargetArtifacts gpuArt = compileResampleMixGpuTargets(mode);
    verifyGpuArtifactsValid(gpuArt, label);
    std::printf("  [gpu-mir] %-14s portable=1, spirv=%zu words (%u B push), ptx=%zu B, msl=%zu B\n",
                label, gpuArt.spirv.words.size(), gpuArt.spirv.push_constant_bytes,
                gpuArt.ptx.size(), gpuArt.msl.source.size());

    // 3. Vulkan GPU execution parity vs CPU (when Vulkan is available)
    if (brass::gpu::vulkan_available()) {
        const auto* caps = brass::gpu::vulkan_device_caps();
        std::printf("  [vulkan]  device=%s (%s)\n",
                    caps ? caps->info.name.c_str() : "unknown",
                    caps ? caps->driver_name.c_str() : "unknown");
        std::vector<float> gpuOut;
        std::string vkErr;
        bool ok = runResampleMixVulkan(
            gpuArt.spirv,
            input.data(),
            inFrames,
            outFrames,
            filterTable.empty() ? nullptr : filterTable.data(),
            filterTable.size(),
            params,
            gpuOut,
            &vkErr);
        if (!ok) {
            std::printf("  [vulkan]  error: %s\n", vkErr.c_str());
        }
        ASSERT_TRUE(ok);
        ASSERT_EQ(gpuOut.size(), cpuOut.size());
        float gpuCpuDiff = maxAbsDiff(gpuOut, cpuOut);
        std::printf("  [vulkan]  %-14s max abs diff GPU vs CPU = %.3g\n",
                    label, static_cast<double>(gpuCpuDiff));
        ASSERT_LT(gpuCpuDiff, 1e-4f);
    } else {
        std::printf("  [vulkan]  unavailable (%s) - skipped device execution\n",
                    brass::gpu::vulkan_last_error().c_str());
    }
}

} // namespace

TEST(resampler_mir_linear_cpu_gpu_parity)
{
    runModeParityAndGpuTest(MirResampleMode::Linear, "linear", 48000, 44100, 512);
    PASS();
}

TEST(resampler_mir_cubic_cpu_gpu_parity)
{
    runModeParityAndGpuTest(MirResampleMode::Cubic, "cubic", 44100, 48000, 512);
    PASS();
}

TEST(resampler_mir_windowed_sinc_cpu_gpu_parity)
{
    runModeParityAndGpuTest(MirResampleMode::WindowedSinc, "windowed_sinc", 48000, 44100, 512);
    PASS();
}

TEST(resampler_mir_sinc_matches_broaudio_resample)
{
    constexpr int inRate = 48000;
    constexpr int outRate = 44100;
    constexpr int inFrames = 512;
    int outFrames = static_cast<int>(std::ceil(static_cast<double>(inFrames) * outRate / inRate));

    auto input = makeStereoTestSignal(inFrames, inRate);
    auto table = buildMirSincFilterTable(inRate, outRate);
    auto cpuKernel = compileResampleMixCpuKernel(MirResampleMode::WindowedSinc);
    auto cpuFn = cpuKernel->as<ResampleMixCpuFn>();

    std::vector<float> mirOut(static_cast<size_t>(outFrames) * 2, 0.0f);
    float step = static_cast<float>(static_cast<double>(inRate) / static_cast<double>(outRate));
    cpuFn(input.data(), table.data(), mirOut.data(), inFrames, outFrames,
          step, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f);

    std::vector<float> refResample = resample(input.data(), inFrames, 2, inRate, outRate);
    ASSERT_EQ(mirOut.size(), refResample.size());
    float diff = maxAbsDiff(mirOut, refResample);
    std::printf("  [cpu-mir] sinc vs broaudio::resample (f64 phase) max abs diff = %.3g\n",
                static_cast<double>(diff));
    ASSERT_LT(diff, 5e-4f);
    PASS();
}

TEST(stereo_mix_mir_cpu_gpu_parity)
{
    constexpr int numFrames = 1024;
    auto input = makeStereoTestSignal(numFrames, 48000);

    BatchDspMixParams params;
    params.gain = 0.90f;
    params.panL = 0.85f;
    params.panR = 0.52f;
    params.softClipDrive = 3.0f;
    params.softClipMix = 0.85f;

    auto cpuKernel = compileStereoMixCpuKernel();
    ASSERT_TRUE(cpuKernel != nullptr && cpuKernel->is_valid());
    auto cpuFn = cpuKernel->as<StereoMixCpuFn>();
    ASSERT_TRUE(cpuFn != nullptr);

    std::vector<float> cpuOut(static_cast<size_t>(numFrames) * 2, 0.0f);
    cpuFn(input.data(), cpuOut.data(), numFrames,
          params.gain, params.panL, params.panR, params.softClipDrive, params.softClipMix);

    std::vector<float> refOut = stereoMixReference(input.data(), numFrames, params);
    float cpuRefDiff = maxAbsDiff(cpuOut, refOut);
    std::printf("  [cpu-mir] stereo_mix     max abs diff vs scalar ref = %.3g\n",
                static_cast<double>(cpuRefDiff));
    ASSERT_LT(cpuRefDiff, 1e-5f);

    GpuTargetArtifacts gpuArt = compileStereoMixGpuTargets();
    verifyGpuArtifactsValid(gpuArt, "stereo_mix");
    std::printf("  [gpu-mir] stereo_mix     portable=1, spirv=%zu words (%u B push), ptx=%zu B, msl=%zu B\n",
                gpuArt.spirv.words.size(), gpuArt.spirv.push_constant_bytes,
                gpuArt.ptx.size(), gpuArt.msl.source.size());

    if (brass::gpu::vulkan_available()) {
        std::vector<float> gpuOut;
        std::string vkErr;
        bool ok = runStereoMixVulkan(
            gpuArt.spirv, input.data(), numFrames, params, gpuOut, &vkErr);
        if (!ok) std::printf("  [vulkan]  error: %s\n", vkErr.c_str());
        ASSERT_TRUE(ok);
        float gpuCpuDiff = maxAbsDiff(gpuOut, cpuOut);
        std::printf("  [vulkan]  stereo_mix     max abs diff GPU vs CPU = %.3g\n",
                    static_cast<double>(gpuCpuDiff));
        ASSERT_LT(gpuCpuDiff, 1e-4f);
    }
    PASS();
}

TEST(gpu_portability_rejects_fp64_by_default)
{
    brass::Module mod("fp64_reject_check");
    brass::Function* fn = mod.create_function(
        "bad_fp64_dsp", brass::Type::void_type(), {brass::Type::ptr(), brass::Type::f64()});
    brass::Builder b(mod);
    b.set_function(fn);
    brass::BasicBlock* entry = b.append_block("entry");
    brass::Value* ptr = b.add_block_param(entry, brass::Type::ptr());
    brass::Value* val = b.add_block_param(entry, brass::Type::f64());
    b.position_at_end(entry);
    b.build_store(brass::Type::f64(), ptr, 0, val);
    b.build_ret_void();
    fn->rebuild_cfg_predecessors();

    brass::DiagnosticReporter diag;
    ASSERT_FALSE(brass::verify_gpu_portability(*fn, {}, &diag));
    ASSERT_TRUE(diag.has_errors());
    PASS();
}

#endif // BROAUDIO_HAS_JIT

int main()
{
    return runAllTests();
}
