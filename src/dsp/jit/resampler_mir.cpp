#include "resampler_mir.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#if BROAUDIO_HAS_JIT
#include "audio_native_cache.h"

#include <brass/codegen/kernel_jit.hpp>
#include <brass/core/diagnostics.hpp>
#include <brass/gpu/vulkan_driver.hpp>
#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/gpu_opt.hpp>
#include <brass/mir/gpu_portability.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/types.hpp>
#endif

namespace broaudio {

namespace {

double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    double halfX = x * 0.5;
    for (int k = 1; k < 30; ++k) {
        term *= (halfX / k) * (halfX / k);
        sum += term;
        if (term < 1e-20 * sum) break;
    }
    return sum;
}

int clampIndex(int idx, int maxIdx) noexcept
{
    if (idx < 0) return 0;
    if (idx > maxIdx) return maxIdx;
    return idx;
}

void applyStereoMixScalar(
    float xL,
    float xR,
    const BatchDspMixParams& p,
    float& outL,
    float& outR) noexcept
{
    float wetL = std::tanh(xL * p.softClipDrive);
    float wetR = std::tanh(xR * p.softClipDrive);
    float shapedL = xL + (wetL - xL) * p.softClipMix;
    float shapedR = xR + (wetR - xR) * p.softClipMix;
    outL = shapedL * p.gain * p.panL;
    outR = shapedR * p.gain * p.panR;
}

#if BROAUDIO_HAS_JIT

using brass::BasicBlock;
using brass::Builder;
using brass::Function;
using brass::Module;
using brass::Type;
using brass::Value;
using brass::codegen::KernelBuilder;
using brass::codegen::KernelFunction;
using brass::codegen::KernelOptions;

const char* resampleModeSuffix(MirResampleMode mode) noexcept
{
    switch (mode) {
        case MirResampleMode::Linear: return "linear";
        case MirResampleMode::Cubic: return "cubic";
        case MirResampleMode::WindowedSinc: return "sinc";
    }
    return "linear";
}

Value* clampI32(Builder& b, Value* idx, Value* zero, Value* maxVal)
{
    Value* lo = b.build_select(b.build_slt(idx, zero), zero, idx);
    return b.build_select(b.build_sgt(lo, maxVal), maxVal, lo);
}

// Pure f32/i32 Brass MIR tanh(x) with zero external calls and zero FP64.
// tanh(x) = 1 - 2 / (exp2(clamp(2 * log2(e) * x, -20, 20)) + 1)
// Range-reduced around integer nearest via floor(v + 0.5) so |z| <= 0.5 * ln(2),
// giving ~1e-7 accuracy from a degree-6 Taylor polynomial.
Value* emitPureTanhF32(Builder& b, Value* x)
{
    Value* v = b.build_fmin_f32(
        b.build_fmax_f32(b.build_mul(x, b.build_fconst_f32(2.88539008178f)), b.build_fconst_f32(-20.0f)),
        b.build_fconst_f32(20.0f));
    Value* ipF = b.build_floor_f32(b.build_add(v, b.build_fconst_f32(0.5f)));
    Value* fp = b.build_sub(v, ipF);
    Value* z = b.build_mul(fp, b.build_fconst_f32(0.69314718056f));

    Value* p = b.build_fconst_f32(1.0f / 720.0f);
    p = b.build_add(b.build_fconst_f32(1.0f / 120.0f), b.build_mul(z, p));
    p = b.build_add(b.build_fconst_f32(1.0f / 24.0f), b.build_mul(z, p));
    p = b.build_add(b.build_fconst_f32(1.0f / 6.0f), b.build_mul(z, p));
    p = b.build_add(b.build_fconst_f32(0.5f), b.build_mul(z, p));
    p = b.build_add(b.build_fconst_f32(1.0f), b.build_mul(z, p));
    p = b.build_add(b.build_fconst_f32(1.0f), b.build_mul(z, p));

    Value* biased = b.build_add(b.build_fptosi_i32_f32(ipF), b.build_iconst_i32(127));
    Value* scale = b.build_bitcast_f32_i32(b.build_shl(biased, b.build_iconst_i32(23)));
    Value* exp2v = b.build_mul(scale, p);
    Value* denom = b.build_add(exp2v, b.build_fconst_f32(1.0f));
    return b.build_sub(b.build_fconst_f32(1.0f), b.build_sdiv(b.build_fconst_f32(2.0f), denom));
}

void emitStereoGainPanSoftClipStore(
    KernelBuilder& kb,
    Value* frameIdx,
    Value* xL,
    Value* xR,
    Value* outBuf,
    Value* gain,
    Value* panL,
    Value* panR,
    Value* drive,
    Value* softClipMix)
{
    Builder& b = kb.builder();
    Value* wetL = emitPureTanhF32(b, b.build_mul(xL, drive));
    Value* wetR = emitPureTanhF32(b, b.build_mul(xR, drive));
    Value* shapedL = b.build_add(xL, b.build_mul(b.build_sub(wetL, xL), softClipMix));
    Value* shapedR = b.build_add(xR, b.build_mul(b.build_sub(wetR, xR), softClipMix));

    Value* outL = b.build_mul(b.build_mul(shapedL, gain), panL);
    Value* outR = b.build_mul(b.build_mul(shapedR, gain), panR);

    Value* outIdxL = b.build_add(frameIdx, frameIdx);
    Value* outIdxR = b.build_add(outIdxL, kb.const_i32(1));
    kb.store_f32_indexed(outBuf, outIdxL, outL, 4);
    kb.store_f32_indexed(outBuf, outIdxR, outR, 4);
}

void loadStereoSample(KernelBuilder& kb, Value* inBuf, Value* frameIdx, Value*& outL, Value*& outR)
{
    Builder& b = kb.builder();
    Value* idxL = b.build_add(frameIdx, frameIdx);
    Value* idxR = b.build_add(idxL, kb.const_i32(1));
    outL = kb.load_f32_indexed(inBuf, idxL, 4);
    outR = kb.load_f32_indexed(inBuf, idxR, 4);
}

Value* catmullRomSample(Builder& b, Value* y0, Value* y1, Value* y2, Value* y3, Value* t)
{
    Value* c0 = y1;
    Value* c1 = b.build_mul(b.build_fconst_f32(0.5f), b.build_sub(y2, y0));
    Value* c2 = b.build_sub(
        b.build_add(y0, b.build_mul(b.build_fconst_f32(2.0f), y2)),
        b.build_add(b.build_mul(b.build_fconst_f32(2.5f), y1), b.build_mul(b.build_fconst_f32(0.5f), y3)));
    Value* c3 = b.build_add(
        b.build_mul(b.build_fconst_f32(0.5f), b.build_sub(y3, y0)),
        b.build_mul(b.build_fconst_f32(1.5f), b.build_sub(y1, y2)));
    Value* acc = b.build_add(b.build_mul(c3, t), c2);
    acc = b.build_add(b.build_mul(acc, t), c1);
    return b.build_add(b.build_mul(acc, t), c0);
}

// Single-source MIR body for batch resampling + stereo gain/pan/soft-clipper mix.
void emitResampleMixFrame(
    KernelBuilder& kb,
    MirResampleMode mode,
    Value* frameIdx,
    Value* inBuf,
    Value* filterTable,
    Value* outBuf,
    Value* inFrames,
    Value* step,
    Value* gain,
    Value* panL,
    Value* panR,
    Value* drive,
    Value* softClipMix)
{
    Builder& b = kb.builder();
    Value* c0 = kb.const_i32(0);
    Value* c1 = kb.const_i32(1);
    Value* maxInIdx = b.build_sub(inFrames, c1);

    Value* frameF = b.build_sitofp_f32_i32(frameIdx);
    Value* pos = b.build_mul(frameF, step);
    Value* posFloor = b.build_floor_f32(pos);
    Value* intPos = b.build_fptosi_i32_f32(posFloor);
    Value* frac = b.build_sub(pos, posFloor);

    Value* xL = nullptr;
    Value* xR = nullptr;

    if (mode == MirResampleMode::Linear) {
        Value* idx0 = clampI32(b, intPos, c0, maxInIdx);
        Value* idx1 = clampI32(b, b.build_add(intPos, c1), c0, maxInIdx);
        Value* s0L = nullptr;
        Value* s0R = nullptr;
        Value* s1L = nullptr;
        Value* s1R = nullptr;
        loadStereoSample(kb, inBuf, idx0, s0L, s0R);
        loadStereoSample(kb, inBuf, idx1, s1L, s1R);
        xL = b.build_add(s0L, b.build_mul(b.build_sub(s1L, s0L), frac));
        xR = b.build_add(s0R, b.build_mul(b.build_sub(s1R, s0R), frac));
    } else if (mode == MirResampleMode::Cubic) {
        Value* idx0 = clampI32(b, b.build_sub(intPos, c1), c0, maxInIdx);
        Value* idx1 = clampI32(b, intPos, c0, maxInIdx);
        Value* idx2 = clampI32(b, b.build_add(intPos, c1), c0, maxInIdx);
        Value* idx3 = clampI32(b, b.build_add(intPos, kb.const_i32(2)), c0, maxInIdx);
        Value* y0L = nullptr;
        Value* y0R = nullptr;
        Value* y1L = nullptr;
        Value* y1R = nullptr;
        Value* y2L = nullptr;
        Value* y2R = nullptr;
        Value* y3L = nullptr;
        Value* y3R = nullptr;
        loadStereoSample(kb, inBuf, idx0, y0L, y0R);
        loadStereoSample(kb, inBuf, idx1, y1L, y1R);
        loadStereoSample(kb, inBuf, idx2, y2L, y2R);
        loadStereoSample(kb, inBuf, idx3, y3L, y3R);
        xL = catmullRomSample(b, y0L, y1L, y2L, y3L, frac);
        xR = catmullRomSample(b, y0R, y1R, y2R, y3R, frac);
    } else {
        Value* phaseRaw = b.build_fptosi_i32_f32(
            b.build_mul(frac, b.build_fconst_f32(static_cast<float>(kMirSincNumPhases))));
        Value* phase = clampI32(b, phaseRaw, c0, kb.const_i32(kMirSincNumPhases - 1));
        Value* tableBase = b.build_mul(phase, kb.const_i32(kMirSincKernelSize));
        Value* base = b.build_sub(intPos, kb.const_i32(kMirSincHalfTaps - 1));

        Value* z0 = kb.const_f32(0.0f);
        std::vector<Value*> sums = kb.for_range_reduce_n(
            c0, kb.const_i32(kMirSincKernelSize), c1, {z0, z0},
            [&](Value* t, const std::vector<Value*>& acc) -> std::vector<Value*> {
                Value* srcIdx = clampI32(b, b.build_add(base, t), c0, maxInIdx);
                Value* sL = nullptr;
                Value* sR = nullptr;
                loadStereoSample(kb, inBuf, srcIdx, sL, sR);
                Value* coeff = kb.load_f32_indexed(filterTable, b.build_add(tableBase, t), 4);
                return {
                    b.build_add(acc[0], b.build_mul(sL, coeff)),
                    b.build_add(acc[1], b.build_mul(sR, coeff)),
                };
            });
        xL = sums[0];
        xR = sums[1];
    }

    emitStereoGainPanSoftClipStore(kb, frameIdx, xL, xR, outBuf, gain, panL, panR, drive, softClipMix);
}

// Single-source MIR body for 1:1 batch stereo gain/pan/soft-clipper mix.
void emitStereoMixFrame(
    KernelBuilder& kb,
    Value* frameIdx,
    Value* inBuf,
    Value* outBuf,
    Value* gain,
    Value* panL,
    Value* panR,
    Value* drive,
    Value* softClipMix)
{
    Value* xL = nullptr;
    Value* xR = nullptr;
    loadStereoSample(kb, inBuf, frameIdx, xL, xR);
    emitStereoGainPanSoftClipStore(kb, frameIdx, xL, xR, outBuf, gain, panL, panR, drive, softClipMix);
}

Function* buildResampleMixMirImpl(MirResampleMode mode, Module& mod, bool isGpu)
{
    std::string fnName = std::string(isGpu ? "resampler_mix_gpu_" : "resampler_mix_cpu_") +
                         resampleModeSuffix(mode);
    Function* fn = mod.create_function(fnName, Type::void_type(), {
        Type::ptr(), // inBuf
        Type::ptr(), // filterTable
        Type::ptr(), // outBuf
        Type::i32(), // inFrames
        Type::i32(), // outFrames
        Type::f32(), // step
        Type::f32(), // gain
        Type::f32(), // panL
        Type::f32(), // panR
        Type::f32(), // drive
        Type::f32(), // softClipMix
    });

    KernelBuilder kb(mod, fn);
    BasicBlock* entry = kb.builder().append_block("entry");
    Value* inBuf       = kb.builder().add_block_param(entry, Type::ptr());
    Value* filterTable = kb.builder().add_block_param(entry, Type::ptr());
    Value* outBuf      = kb.builder().add_block_param(entry, Type::ptr());
    Value* inFrames    = kb.builder().add_block_param(entry, Type::i32());
    Value* outFrames   = kb.builder().add_block_param(entry, Type::i32());
    Value* step        = kb.builder().add_block_param(entry, Type::f32());
    Value* gain        = kb.builder().add_block_param(entry, Type::f32());
    Value* panL        = kb.builder().add_block_param(entry, Type::f32());
    Value* panR        = kb.builder().add_block_param(entry, Type::f32());
    Value* drive       = kb.builder().add_block_param(entry, Type::f32());
    Value* softClipMix = kb.builder().add_block_param(entry, Type::f32());
    kb.position_at_end(entry);

    if (isGpu) {
        Value* tid = kb.global_tid_x();
        Value* inBounds = kb.builder().build_and(
            kb.builder().build_slt(tid, outFrames),
            kb.builder().build_sgt(inFrames, kb.const_i32(0)));
        kb.if_then(inBounds, [&]() {
            emitResampleMixFrame(
                kb, mode, tid, inBuf, filterTable, outBuf, inFrames,
                step, gain, panL, panR, drive, softClipMix);
        });
    } else {
        kb.if_then(kb.builder().build_sgt(inFrames, kb.const_i32(0)), [&]() {
            kb.for_range(kb.const_i32(0), outFrames, kb.const_i32(1), [&](Value* idx) {
                emitResampleMixFrame(
                    kb, mode, idx, inBuf, filterTable, outBuf, inFrames,
                    step, gain, panL, panR, drive, softClipMix);
            });
        });
    }

    kb.builder().build_ret_void();
    fn->rebuild_cfg_predecessors();
    return fn;
}

Function* buildStereoMixMirImpl(Module& mod, bool isGpu)
{
    const char* fnName = isGpu ? "stereo_mix_gpu" : "stereo_mix_cpu";
    Function* fn = mod.create_function(fnName, Type::void_type(), {
        Type::ptr(), // inBuf
        Type::ptr(), // outBuf
        Type::i32(), // numFrames
        Type::f32(), // gain
        Type::f32(), // panL
        Type::f32(), // panR
        Type::f32(), // drive
        Type::f32(), // softClipMix
    });

    KernelBuilder kb(mod, fn);
    BasicBlock* entry = kb.builder().append_block("entry");
    Value* inBuf       = kb.builder().add_block_param(entry, Type::ptr());
    Value* outBuf      = kb.builder().add_block_param(entry, Type::ptr());
    Value* numFrames   = kb.builder().add_block_param(entry, Type::i32());
    Value* gain        = kb.builder().add_block_param(entry, Type::f32());
    Value* panL        = kb.builder().add_block_param(entry, Type::f32());
    Value* panR        = kb.builder().add_block_param(entry, Type::f32());
    Value* drive       = kb.builder().add_block_param(entry, Type::f32());
    Value* softClipMix = kb.builder().add_block_param(entry, Type::f32());
    kb.position_at_end(entry);

    if (isGpu) {
        Value* tid = kb.global_tid_x();
        kb.if_then(kb.builder().build_slt(tid, numFrames), [&]() {
            emitStereoMixFrame(kb, tid, inBuf, outBuf, gain, panL, panR, drive, softClipMix);
        });
    } else {
        kb.for_range(kb.const_i32(0), numFrames, kb.const_i32(1), [&](Value* idx) {
            emitStereoMixFrame(kb, idx, inBuf, outBuf, gain, panL, panR, drive, softClipMix);
        });
    }

    kb.builder().build_ret_void();
    fn->rebuild_cfg_predecessors();
    return fn;
}

KernelOptions makeCpuSimdKernelOptions()
{
    KernelOptions opts;
    opts.opt_level = 2;
    opts.enable_optimizations = true;
    opts.enable_sroa = true;
    opts.enable_bce = true;
    opts.enable_smt_guard_elim = true;
    opts.enable_superopt = true;
    opts.enable_avx2 = true;
    opts.enable_fma = true;
    opts.enable_fp_reassociation = true;
    opts.enable_vectorize = true;
    opts.enable_slp = true;
    opts.enable_unroll = true;
    opts.unroll_factor = 4;
    opts.enable_parallel = false;
    opts.enable_ftz_daz = true;
    return opts;
}

GpuTargetArtifacts compileGpuTargetsFromMir(Function* fn, Module& mod, uint32_t localSizeX)
{
    GpuTargetArtifacts out;
    brass::GpuOptOptions gopt;
    gopt.enable_gpu_superopt = true;
    gopt.enable_small_loop_unroll = true;
    gopt.enable_load_coalesce = true;
    brass::optimize_gpu_function(*fn, mod, gopt);
    fn->rebuild_cfg_predecessors();

    brass::DiagnosticReporter diag;
    brass::GpuPortabilityOptions portOpts;
    portOpts.allow_fp64 = false;
    out.portable = brass::verify_gpu_portability(*fn, portOpts, &diag);
    out.portabilityDiagnostics = diag.format_all();

    brass::target::SpirvOptions spvOpts;
    spvOpts.local_size_x = localSizeX;
    spvOpts.local_size_y = 1;
    spvOpts.local_size_z = 1;
    out.spirv = brass::target::SpirvTarget::compile(*fn, spvOpts);

    brass::target::PtxOptions ptxOpts;
    out.ptx = brass::target::PtxTarget::emit_function(*fn, ptxOpts);

    brass::target::MslOptions mslOpts;
    mslOpts.workgroup_x = localSizeX;
    mslOpts.workgroup_y = 1;
    mslOpts.workgroup_z = 1;
    out.msl = brass::target::MslTarget::compile(*fn, mslOpts);
    return out;
}

#endif // BROAUDIO_HAS_JIT

} // namespace

std::vector<float> buildMirSincFilterTable(int inputRate, int outputRate)
{
    if (inputRate <= 0 || outputRate <= 0) return {};

    double ratio = static_cast<double>(outputRate) / static_cast<double>(inputRate);
    double cutoff = std::min(1.0, ratio) * 0.95;

    std::vector<float> table(static_cast<size_t>(kMirSincNumPhases) * kMirSincKernelSize);
    double invI0Beta = 1.0 / besselI0(kMirSincKaiserBeta);

    for (int p = 0; p < kMirSincNumPhases; ++p) {
        double frac = static_cast<double>(p) / kMirSincNumPhases;
        float* row = table.data() + static_cast<size_t>(p) * kMirSincKernelSize;
        double sum = 0.0;

        for (int t = 0; t < kMirSincKernelSize; ++t) {
            double x = static_cast<double>(t - kMirSincHalfTaps) + (1.0 - frac);
            double sincVal = (std::fabs(x) < 1e-12)
                ? cutoff
                : (std::sin(std::numbers::pi * cutoff * x) / (std::numbers::pi * x));
            double wPos = static_cast<double>(t) / static_cast<double>(kMirSincKernelSize - 1);
            double wArg = 2.0 * wPos - 1.0;
            double kaiser = besselI0(kMirSincKaiserBeta * std::sqrt(std::max(0.0, 1.0 - wArg * wArg))) *
                            invI0Beta;
            row[t] = static_cast<float>(sincVal * kaiser);
            sum += row[t];
        }

        if (std::fabs(sum) > 1e-12) {
            float invSum = static_cast<float>(1.0 / sum);
            for (int t = 0; t < kMirSincKernelSize; ++t) {
                row[t] *= invSum;
            }
        }
    }

    return table;
}

std::vector<float> resampleMixReference(
    const float* inputInterleavedStereo,
    int inputFrames,
    int outputFrames,
    MirResampleMode mode,
    const float* filterTable,
    const BatchDspMixParams& params)
{
    if (!inputInterleavedStereo || inputFrames <= 0 || outputFrames <= 0) return {};
    if (mode == MirResampleMode::WindowedSinc && !filterTable) return {};

    std::vector<float> out(static_cast<size_t>(outputFrames) * 2, 0.0f);
    const int maxIdx = inputFrames - 1;

    for (int outIdx = 0; outIdx < outputFrames; ++outIdx) {
        float pos = static_cast<float>(outIdx) * params.step;
        float posFl = std::floor(pos);
        int intPos = static_cast<int>(posFl);
        float frac = pos - posFl;

        float xL = 0.0f;
        float xR = 0.0f;

        if (mode == MirResampleMode::Linear) {
            int i0 = clampIndex(intPos, maxIdx);
            int i1 = clampIndex(intPos + 1, maxIdx);
            float s0L = inputInterleavedStereo[i0 * 2 + 0];
            float s0R = inputInterleavedStereo[i0 * 2 + 1];
            float s1L = inputInterleavedStereo[i1 * 2 + 0];
            float s1R = inputInterleavedStereo[i1 * 2 + 1];
            xL = s0L + (s1L - s0L) * frac;
            xR = s0R + (s1R - s0R) * frac;
        } else if (mode == MirResampleMode::Cubic) {
            int i0 = clampIndex(intPos - 1, maxIdx);
            int i1 = clampIndex(intPos, maxIdx);
            int i2 = clampIndex(intPos + 1, maxIdx);
            int i3 = clampIndex(intPos + 2, maxIdx);
            auto evalCubic = [frac](float y0, float y1, float y2, float y3) {
                float c0 = y1;
                float c1 = 0.5f * (y2 - y0);
                float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                return ((c3 * frac + c2) * frac + c1) * frac + c0;
            };
            xL = evalCubic(
                inputInterleavedStereo[i0 * 2 + 0],
                inputInterleavedStereo[i1 * 2 + 0],
                inputInterleavedStereo[i2 * 2 + 0],
                inputInterleavedStereo[i3 * 2 + 0]);
            xR = evalCubic(
                inputInterleavedStereo[i0 * 2 + 1],
                inputInterleavedStereo[i1 * 2 + 1],
                inputInterleavedStereo[i2 * 2 + 1],
                inputInterleavedStereo[i3 * 2 + 1]);
        } else {
            int phase = clampIndex(static_cast<int>(frac * static_cast<float>(kMirSincNumPhases)),
                                   kMirSincNumPhases - 1);
            const float* coeffs = filterTable + static_cast<size_t>(phase) * kMirSincKernelSize;
            int base = intPos - kMirSincHalfTaps + 1;
            for (int t = 0; t < kMirSincKernelSize; ++t) {
                int srcIdx = clampIndex(base + t, maxIdx);
                float c = coeffs[t];
                xL += inputInterleavedStereo[srcIdx * 2 + 0] * c;
                xR += inputInterleavedStereo[srcIdx * 2 + 1] * c;
            }
        }

        applyStereoMixScalar(xL, xR, params, out[outIdx * 2 + 0], out[outIdx * 2 + 1]);
    }

    return out;
}

std::vector<float> stereoMixReference(
    const float* inputInterleavedStereo,
    int numFrames,
    const BatchDspMixParams& params)
{
    if (!inputInterleavedStereo || numFrames <= 0) return {};
    std::vector<float> out(static_cast<size_t>(numFrames) * 2, 0.0f);
    for (int i = 0; i < numFrames; ++i) {
        applyStereoMixScalar(
            inputInterleavedStereo[i * 2 + 0],
            inputInterleavedStereo[i * 2 + 1],
            params,
            out[i * 2 + 0],
            out[i * 2 + 1]);
    }
    return out;
}

#if BROAUDIO_HAS_JIT

Function* buildResampleMixCpuMir(MirResampleMode mode, Module& mod)
{
    return buildResampleMixMirImpl(mode, mod, /*isGpu=*/false);
}

Function* buildResampleMixGpuMir(MirResampleMode mode, Module& mod)
{
    return buildResampleMixMirImpl(mode, mod, /*isGpu=*/true);
}

Function* buildStereoMixCpuMir(Module& mod)
{
    return buildStereoMixMirImpl(mod, /*isGpu=*/false);
}

Function* buildStereoMixGpuMir(Module& mod)
{
    return buildStereoMixMirImpl(mod, /*isGpu=*/true);
}

std::shared_ptr<KernelFunction> compileResampleMixCpuKernel(MirResampleMode mode)
{
    KernelOptions opts = makeCpuSimdKernelOptions();
    std::string fnName = std::string("resampler_mix_cpu_") + resampleModeSuffix(mode);
    std::string modName = std::string("resampler_mix_mod_") + resampleModeSuffix(mode);

    brass::cache::KeyBuilder kb("broaudio.resampler_mir/1");
    kb.add_u64("mode", static_cast<uint64_t>(mode));
    detail::hash_kernel_options(kb, opts);

    KernelFunction kfn = detail::compile_cached_audio_kernel(
        kb.finish(),
        modName.c_str(),
        fnName.c_str(),
        opts,
        [mode](Module& mod) {
            buildResampleMixCpuMir(mode, mod);
        });

    if (!kfn.is_valid()) {
        throw std::runtime_error("compileResampleMixCpuKernel: JIT compilation failed");
    }
    return std::make_shared<KernelFunction>(std::move(kfn));
}

std::shared_ptr<KernelFunction> compileStereoMixCpuKernel()
{
    KernelOptions opts = makeCpuSimdKernelOptions();
    brass::cache::KeyBuilder kb("broaudio.stereo_mix_mir/1");
    detail::hash_kernel_options(kb, opts);

    KernelFunction kfn = detail::compile_cached_audio_kernel(
        kb.finish(),
        "stereo_mix_mod",
        "stereo_mix_cpu",
        opts,
        [](Module& mod) {
            buildStereoMixCpuMir(mod);
        });

    if (!kfn.is_valid()) {
        throw std::runtime_error("compileStereoMixCpuKernel: JIT compilation failed");
    }
    return std::make_shared<KernelFunction>(std::move(kfn));
}

GpuTargetArtifacts compileResampleMixGpuTargets(MirResampleMode mode, uint32_t localSizeX)
{
    Module mod("resampler_mix_gpu_mod");
    Function* fn = buildResampleMixGpuMir(mode, mod);
    return compileGpuTargetsFromMir(fn, mod, localSizeX);
}

GpuTargetArtifacts compileStereoMixGpuTargets(uint32_t localSizeX)
{
    Module mod("stereo_mix_gpu_mod");
    Function* fn = buildStereoMixGpuMir(mod);
    return compileGpuTargetsFromMir(fn, mod, localSizeX);
}

bool runResampleMixVulkan(
    const brass::target::SpirvKernel& kernel,
    const float* inputInterleavedStereo,
    int inputFrames,
    int outputFrames,
    const float* filterTable,
    size_t filterTableSize,
    const BatchDspMixParams& params,
    std::vector<float>& outInterleavedStereo,
    std::string* outError)
{
    using namespace brass::gpu;
    auto fail = [&](const std::string& msg) {
        if (outError) *outError = msg;
        return false;
    };

    if (!vulkan_available()) return fail("Vulkan unavailable: " + vulkan_last_error());
    if (!inputInterleavedStereo || inputFrames <= 0 || outputFrames <= 0) {
        return fail("Invalid input buffer or frame counts");
    }

    const VulkanDeviceCaps* caps = vulkan_device_caps();
    if (!caps) return fail("vulkan_device_caps() returned null");
    std::string missing = caps->missing_for(kernel);
    if (!missing.empty()) return fail("Missing Vulkan device capability: " + missing);

    std::string err;
    VulkanModule vkMod = VulkanModule::load(kernel, &err);
    if (!vkMod.valid()) return fail("VulkanModule::load failed: " + err);

    const size_t inBytes = static_cast<size_t>(inputFrames) * 2 * sizeof(float);
    const size_t outBytes = static_cast<size_t>(outputFrames) * 2 * sizeof(float);
    const size_t tableBytes = std::max<size_t>(filterTableSize * sizeof(float), 4);

    VulkanBuffer inBuf = VulkanBuffer::alloc(inBytes);
    VulkanBuffer tblBuf = VulkanBuffer::alloc(tableBytes);
    VulkanBuffer outBuf = VulkanBuffer::alloc(outBytes);
    if (!inBuf.valid() || !tblBuf.valid() || !outBuf.valid()) {
        return fail("VulkanBuffer::alloc failed: " + vulkan_last_error());
    }
    if (!inBuf.upload(inputInterleavedStereo, inBytes)) {
        return fail("Vulkan input upload failed: " + vulkan_last_error());
    }
    if (filterTable && filterTableSize > 0) {
        if (!tblBuf.upload(filterTable, filterTableSize * sizeof(float))) {
            return fail("Vulkan filterTable upload failed: " + vulkan_last_error());
        }
    } else if (!tblBuf.zero()) {
        return fail("Vulkan filterTable zero failed: " + vulkan_last_error());
    }
    if (!outBuf.zero()) return fail("Vulkan output zero failed: " + vulkan_last_error());

    uint32_t block = kernel.local_size[0] ? kernel.local_size[0] : 256u;
    uint32_t grid = (static_cast<uint32_t>(outputFrames) + block - 1u) / block;
    if (!vkMod.launch_1d(grid, block, {
            inBuf, tblBuf, outBuf,
            static_cast<int32_t>(inputFrames),
            static_cast<int32_t>(outputFrames),
            params.step, params.gain, params.panL, params.panR,
            params.softClipDrive, params.softClipMix}, &err)) {
        return fail("Vulkan launch failed: " + err);
    }

    outInterleavedStereo.resize(static_cast<size_t>(outputFrames) * 2);
    if (!outBuf.download(outInterleavedStereo.data(), outBytes)) {
        return fail("Vulkan output download failed: " + vulkan_last_error());
    }
    return true;
}

bool runStereoMixVulkan(
    const brass::target::SpirvKernel& kernel,
    const float* inputInterleavedStereo,
    int numFrames,
    const BatchDspMixParams& params,
    std::vector<float>& outInterleavedStereo,
    std::string* outError)
{
    using namespace brass::gpu;
    auto fail = [&](const std::string& msg) {
        if (outError) *outError = msg;
        return false;
    };

    if (!vulkan_available()) return fail("Vulkan unavailable: " + vulkan_last_error());
    if (!inputInterleavedStereo || numFrames <= 0) {
        return fail("Invalid input buffer or frame count");
    }

    const VulkanDeviceCaps* caps = vulkan_device_caps();
    if (!caps) return fail("vulkan_device_caps() returned null");
    std::string missing = caps->missing_for(kernel);
    if (!missing.empty()) return fail("Missing Vulkan device capability: " + missing);

    std::string err;
    VulkanModule vkMod = VulkanModule::load(kernel, &err);
    if (!vkMod.valid()) return fail("VulkanModule::load failed: " + err);

    const size_t bytes = static_cast<size_t>(numFrames) * 2 * sizeof(float);
    VulkanBuffer inBuf = VulkanBuffer::alloc(bytes);
    VulkanBuffer outBuf = VulkanBuffer::alloc(bytes);
    if (!inBuf.valid() || !outBuf.valid()) {
        return fail("VulkanBuffer::alloc failed: " + vulkan_last_error());
    }
    if (!inBuf.upload(inputInterleavedStereo, bytes) || !outBuf.zero()) {
        return fail("Vulkan buffer init failed: " + vulkan_last_error());
    }

    uint32_t block = kernel.local_size[0] ? kernel.local_size[0] : 256u;
    uint32_t grid = (static_cast<uint32_t>(numFrames) + block - 1u) / block;
    if (!vkMod.launch_1d(grid, block, {
            inBuf, outBuf,
            static_cast<int32_t>(numFrames),
            params.gain, params.panL, params.panR,
            params.softClipDrive, params.softClipMix}, &err)) {
        return fail("Vulkan launch failed: " + err);
    }

    outInterleavedStereo.resize(static_cast<size_t>(numFrames) * 2);
    if (!outBuf.download(outInterleavedStereo.data(), bytes)) {
        return fail("Vulkan output download failed: " + vulkan_last_error());
    }
    return true;
}

#endif // BROAUDIO_HAS_JIT

} // namespace broaudio
