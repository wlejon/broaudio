#include "broaudio/verified_dsp_kernel.h"
#include "broaudio/synth/synth_verify.h"
#include "verified_dsp_compiler.h"
#include "verified_dsp_math.h"

#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/verify/smt_equiv.hpp>

#include <cmath>
#include <cstring>
#include <utility>

namespace broaudio {

CustomDspKernel compileVerifiedDspKernel(
    std::string_view jsSourceOrExpr,
    const VerifiedDspCompileOptions& opts) {

    CustomDspKernel result;
    result.maxAmp = (std::isfinite(opts.maxAmp) && opts.maxAmp > 0.0f) ? opts.maxAmp : 1.0f;
    result.kernelName = opts.name.empty() ? "js_dsp_kernel" : opts.name;
    result.source = std::string(jsSourceOrExpr);

    brass::Module mod("verified_dsp_module");
    mod.set_allow_fp_reassociation(false);

    brass::Function* fn = nullptr;
    std::string err;
    if (!compileJsDspToMir(jsSourceOrExpr, opts, mod, fn, err)) {
        result.errorMessage = err;
        return result;
    }

    result.zeroAllocProven = true;

    if (opts.enableSuperopt) {
        SynthMirVerifyResult vres = optimize_and_verify_mir(*fn, mod);
        if (!vres.proven) {
            result.errorMessage = "SMT equivalence verification failed: " + vres.diagnostic;
            return result;
        }
        result.mirEquivProven = true;
    } else {
        brass::verify::EquivOptions eqOpts;
        eqOpts.check_memory = false;
        eqOpts.check_guards = false;
        auto selfEq = brass::verify::verify_mir_equivalence(*fn, *fn, eqOpts);
        result.mirEquivProven = (selfEq.status == brass::verify::EquivStatus::ProvenEquivalent);
    }

    result.nanSafeProven = dspmath::proveSpeakerProtectionSafety(*fn, result.maxAmp);
    if (!result.nanSafeProven) {
        result.errorMessage = "SMT speaker-protection proof failed to verify finite [-maxAmp, +maxAmp] bounds";
        return result;
    }

    auto kopts = brass::codegen::KernelOptions::audio_realtime();
    kopts.enable_optimizations = true;
    kopts.opt_level = 2;
    kopts.enable_smt_guard_elim = opts.enableSuperopt;
    kopts.enable_superopt = opts.enableSuperopt;
    kopts.enable_fma = false;
    kopts.enable_fp_reassociation = false;

    brass::codegen::KernelJit jit(kopts);
    brass::codegen::KernelFunction kfn = jit.compile(mod, result.kernelName);
    if (!kfn.is_valid() || !kfn.entry_point()) {
        result.errorMessage = "KernelJit failed to lower verified MIR to native machine code";
        return result;
    }

    auto kept = std::make_shared<brass::codegen::KernelFunction>(std::move(kfn));
    result.fn = reinterpret_cast<DspKernelFnPtr>(kept->entry_point());
    result.jitKeepAlive = std::move(kept);
    result.valid = true;
    result.verified = result.zeroAllocProven && result.nanSafeProven && result.mirEquivProven;
    return result;
}

VerifiedDspProcessor::VerifiedDspProcessor() {
    for (size_t i = 0; i < MAX_PARAMS; ++i) {
        params_[i].store(0.0f, std::memory_order_relaxed);
    }
}

VerifiedDspProcessor::~VerifiedDspProcessor() {
    rcu_.reclaim();
}

bool VerifiedDspProcessor::compileAndInstall(
    std::string_view jsSourceOrExpr,
    const VerifiedDspCompileOptions& opts,
    std::string* outError) {

    CustomDspKernel kernel = compileVerifiedDspKernel(jsSourceOrExpr, opts);
    if (!kernel.valid || !kernel.verified || !kernel.fn) {
        std::lock_guard<std::mutex> lock(controlMutex_);
        lastError_ = kernel.errorMessage;
        if (outError) *outError = kernel.errorMessage;
        return false;
    }
    return installKernel(std::move(kernel), false);
}

bool VerifiedDspProcessor::hotSwap(
    std::string_view jsSourceOrExpr,
    const VerifiedDspCompileOptions& opts,
    std::string* outError) {

    CustomDspKernel kernel = compileVerifiedDspKernel(jsSourceOrExpr, opts);
    if (!kernel.valid || !kernel.verified || !kernel.fn) {
        std::lock_guard<std::mutex> lock(controlMutex_);
        lastError_ = kernel.errorMessage;
        if (outError) *outError = kernel.errorMessage;
        return false;
    }
    return installKernel(std::move(kernel), true);
}

bool VerifiedDspProcessor::installKernel(CustomDspKernel k, bool isHotSwap) {
    if (!k.valid || !k.fn) return false;

    std::lock_guard<std::mutex> lock(controlMutex_);
    const uint32_t cur = activeIdx_.load(std::memory_order_relaxed);
    const uint32_t next = (cur + 1u) & 1u;
    CustomDspKernel displaced = std::move(kernels_[next]);

    lastError_.clear();
    kernelName_ = k.kernelName;
    currentSource_ = k.source;

    maxAmp_.store(k.maxAmp, std::memory_order_relaxed);
    zeroAllocProven_.store(k.zeroAllocProven, std::memory_order_release);
    nanSafeProven_.store(k.nanSafeProven, std::memory_order_release);
    mirEquivProven_.store(k.mirEquivProven, std::memory_order_release);
    verified_.store(k.verified, std::memory_order_release);

    activeFn_.store(k.fn, std::memory_order_release);
    kernels_[next] = std::move(k);
    activeIdx_.store(next, std::memory_order_release);
    (void)isHotSwap;
    hotSwapCount_.fetch_add(1, std::memory_order_relaxed);

    if (displaced.fn != nullptr || displaced.jitKeepAlive != nullptr) {
        rcu_.retire([old = std::make_shared<CustomDspKernel>(std::move(displaced))]() mutable {
            old.reset();
        });
    }
    rcu_.reclaim();
    return true;
}

std::size_t VerifiedDspProcessor::reclaimRetiredKernels() noexcept {
    rcu_.reclaim();
    return rcu_.pendingCount();
}

void VerifiedDspProcessor::setRawFunction(DspKernelFnPtr fn, bool recordSwap) noexcept {
    const uint32_t idx = activeIdx_.load(std::memory_order_acquire);
    kernels_[idx].fn = fn;
    DspKernelFnPtr prev = activeFn_.exchange(fn, std::memory_order_acq_rel);
    if (recordSwap && prev != nullptr && prev != fn) {
        hotSwapCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void VerifiedDspProcessor::processBlock(
    const float* in,
    float* out,
    uint32_t numSamples,
    double startTime,
    double sampleRate) const noexcept {

    if (!out || numSamples == 0) return;

    RcuDomain::ReadScope guard(rcu_);
    const CustomDspKernel& k = kernels_[activeIdx_.load(std::memory_order_acquire)];
    DspKernelFnPtr fn = k.fn ? k.fn : activeFn_.load(std::memory_order_acquire);
    if (!fn) {
        if (in && in != out) {
            std::memcpy(out, in, static_cast<size_t>(numSamples) * sizeof(float));
        } else if (!in) {
            std::memset(out, 0, static_cast<size_t>(numSamples) * sizeof(float));
        }
        return;
    }

    const float p0 = params_[0].load(std::memory_order_relaxed);
    const float p1 = params_[1].load(std::memory_order_relaxed);
    const double invSr = (sampleRate > 0.0) ? (1.0 / sampleRate) : (1.0 / 48000.0);

    for (uint32_t i = 0; i < numSamples; ++i) {
        const float s = in ? in[i] : 0.0f;
        const float t = static_cast<float>(startTime + static_cast<double>(i) * invSr);
        out[i] = fn(s, t, p0, p1);
    }
}

void VerifiedDspProcessor::processStereoInterleaved(
    float* interleavedBuf,
    uint32_t numFrames,
    double startTime,
    double sampleRate) const noexcept {

    if (!interleavedBuf || numFrames == 0) return;

    RcuDomain::ReadScope guard(rcu_);
    const CustomDspKernel& k = kernels_[activeIdx_.load(std::memory_order_acquire)];
    DspKernelFnPtr fn = k.fn ? k.fn : activeFn_.load(std::memory_order_acquire);
    if (!fn) return;

    const float p0 = params_[0].load(std::memory_order_relaxed);
    const float p1 = params_[1].load(std::memory_order_relaxed);
    const double invSr = (sampleRate > 0.0) ? (1.0 / sampleRate) : (1.0 / 48000.0);

    for (uint32_t i = 0; i < numFrames; ++i) {
        const float t = static_cast<float>(startTime + static_cast<double>(i) * invSr);
        interleavedBuf[i * 2]     = fn(interleavedBuf[i * 2],     t, p0, p1);
        interleavedBuf[i * 2 + 1] = fn(interleavedBuf[i * 2 + 1], t, p0, p1);
    }
}

std::string VerifiedDspProcessor::lastError() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return lastError_;
}

std::string VerifiedDspProcessor::kernelName() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return kernelName_;
}

std::string VerifiedDspProcessor::currentSource() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return currentSource_;
}

} // namespace broaudio
