#pragma once

// Live Lock-Free Verified JS Audio DSP Kernels on the Real-Time Audio Thread.
//
// Compiles a strict, statically-checked subset of JavaScript DSP functions or
// expressions into native machine code via Brass MIR, superoptimizes the MIR
// and x64 LIR, formally proves equivalence and NaN/Inf/amplitude-clamp safety
// via the Brass bitvector SMT solver, and hot-swaps the native function pointer
// onto the real-time audio thread with zero locks and zero allocations.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace broaudio {

// Native compiled DSP kernel ABI:
//   (float sample, float time, float p0, float p1) -> float
using DspKernelFnPtr = float (*)(float sample, float time, float p0, float p1);

struct VerifiedDspCompileOptions {
    float maxAmp = 1.0f;            // Hardware speaker-protection clamp bound [-maxAmp, +maxAmp]
    bool enableSuperopt = true;     // Run Brass MIR + x64 LIR superoptimizers & SMT proof
    std::string name = "js_dsp_kernel";
};

struct CustomDspKernel {
    bool valid = false;
    bool verified = false;          // All static + formal SMT proofs succeeded
    bool zeroAllocProven = false;   // Proven free of GC, heap, closures, throws, unbounded loops
    bool nanSafeProven = false;     // SMT-proven finite output in [-maxAmp, +maxAmp] (no NaN/Inf)
    bool mirEquivProven = false;    // SMT-proven Unopt MIR == Superopt MIR equivalence
    float maxAmp = 1.0f;
    std::string kernelName;
    std::string source;
    std::string errorMessage;
    DspKernelFnPtr fn = nullptr;
    std::shared_ptr<void> jitKeepAlive;

    float eval(float sample, float time = 0.0f, float p0 = 0.0f, float p1 = 0.0f) const noexcept {
        return fn ? fn(sample, time, p0, p1) : sample;
    }
};

// Compile and formally verify a JS DSP function or expression into a native
// machine-code kernel with hardware speaker-protection epilogue.
CustomDspKernel compileVerifiedDspKernel(
    std::string_view jsSourceOrExpr,
    const VerifiedDspCompileOptions& opts = {});

// Real-time lock-free audio processor executing a verified native DSP kernel.
class VerifiedDspProcessor {
public:
    static constexpr size_t MAX_PARAMS = 4;

    VerifiedDspProcessor();
    ~VerifiedDspProcessor() = default;

    VerifiedDspProcessor(const VerifiedDspProcessor&) = delete;
    VerifiedDspProcessor& operator=(const VerifiedDspProcessor&) = delete;

    // Compile a JS kernel and install it as the active kernel.
    bool compileAndInstall(
        std::string_view jsSourceOrExpr,
        const VerifiedDspCompileOptions& opts = {},
        std::string* outError = nullptr);

    // Compile a new JS kernel on the control/JS thread and atomically hot-swap
    // its native function pointer onto the audio thread without locks or clicks.
    // If compilation or verification fails, the existing active kernel remains
    // untouched and false is returned.
    bool hotSwap(
        std::string_view jsSourceOrExpr,
        const VerifiedDspCompileOptions& opts = {},
        std::string* outError = nullptr);

    // Install a pre-compiled CustomDspKernel.
    bool installKernel(const CustomDspKernel& kernel, bool isHotSwap = false);

    // Directly swap a raw native C function pointer (lock-free).
    void setRawFunction(DspKernelFnPtr fn, bool recordSwap = true) noexcept;

    DspKernelFnPtr activeFunction() const noexcept {
        return activeFn_.load(std::memory_order_acquire);
    }

    // Lock-free parameter modulation from the JS/main thread.
    void setParam(size_t index, float value) noexcept {
        if (index < MAX_PARAMS) {
            params_[index].store(value, std::memory_order_relaxed);
        }
    }

    float getParam(size_t index) const noexcept {
        if (index < MAX_PARAMS) {
            return params_[index].load(std::memory_order_relaxed);
        }
        return 0.0f;
    }

    // Evaluate a single sample using the currently active kernel and atomic params.
    float evalSample(float sample, float time = 0.0f) const noexcept {
        DspKernelFnPtr fn = activeFn_.load(std::memory_order_acquire);
        if (!fn) return sample;
        float p0 = params_[0].load(std::memory_order_relaxed);
        float p1 = params_[1].load(std::memory_order_relaxed);
        return fn(sample, time, p0, p1);
    }

    float evalSampleWithParams(float sample, float time, float p0, float p1) const noexcept {
        DspKernelFnPtr fn = activeFn_.load(std::memory_order_acquire);
        if (!fn) return sample;
        return fn(sample, time, p0, p1);
    }

    // Real-time audio thread block processing (zero locks, zero allocations).
    void processBlock(
        const float* in,
        float* out,
        uint32_t numSamples,
        double startTime = 0.0,
        double sampleRate = 48000.0) const noexcept;

    // Real-time stereo interleaved in-place processing for Bus / Engine integration.
    void processStereoInterleaved(
        float* interleavedBuf,
        uint32_t numFrames,
        double startTime = 0.0,
        double sampleRate = 48000.0) const noexcept;

    uint32_t hotSwapCount() const noexcept {
        return hotSwapCount_.load(std::memory_order_relaxed);
    }

    bool isVerified() const noexcept {
        return verified_.load(std::memory_order_acquire);
    }

    bool isZeroAllocProven() const noexcept {
        return zeroAllocProven_.load(std::memory_order_acquire);
    }

    bool isNanSafeProven() const noexcept {
        return nanSafeProven_.load(std::memory_order_acquire);
    }

    bool isMirEquivProven() const noexcept {
        return mirEquivProven_.load(std::memory_order_acquire);
    }

    float maxAmp() const noexcept {
        return maxAmp_.load(std::memory_order_relaxed);
    }

    std::string lastError() const;
    std::string kernelName() const;
    std::string currentSource() const;

private:
    std::atomic<DspKernelFnPtr> activeFn_{nullptr};
    std::atomic<float> params_[MAX_PARAMS];
    std::atomic<uint32_t> hotSwapCount_{0};
    std::atomic<bool> verified_{false};
    std::atomic<bool> zeroAllocProven_{false};
    std::atomic<bool> nanSafeProven_{false};
    std::atomic<bool> mirEquivProven_{false};
    std::atomic<float> maxAmp_{1.0f};

    mutable std::mutex controlMutex_;
    std::vector<CustomDspKernel> retainedKernels_;
    std::string lastError_;
    std::string kernelName_;
    std::string currentSource_;
};

using CustomJitDspNode = VerifiedDspProcessor;

} // namespace broaudio
