#include "test_harness.h"
#include "broaudio/verified_dsp_kernel.h"
#include "broaudio/engine.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

using namespace broaudio;

#if BROAUDIO_HAS_JIT

TEST(verified_dsp_compile_and_smt_proofs)
{
    // 1. Soft-clipping sine-modulated waveshaper kernel
    auto k = compileVerifiedDspKernel(
        "(sample, time, drive, mix) => {"
        "  \"@kernel\";"
        "  const shaped = Math.tanh(sample * drive + Math.sin(time * 440.0 * 6.2831853) * 0.25);"
        "  return sample * (1.0 - mix) + shaped * mix;"
        "}");
    if (!k.valid || !k.verified) {
        std::printf("  compile error: %s\n", k.errorMessage.c_str());
    }
    ASSERT_TRUE(k.valid);
    ASSERT_TRUE(k.verified);
    ASSERT_TRUE(k.zeroAllocProven);
    ASSERT_TRUE(k.nanSafeProven);
    ASSERT_TRUE(k.mirEquivProven);
    ASSERT_TRUE(k.fn != nullptr);

    float y0 = k.eval(0.5f, 0.0f, 2.0f, 1.0f);
    ASSERT_TRUE(std::isfinite(y0));
    ASSERT_NEAR(y0, std::tanh(1.0f), 0.05f);
    PASS();
}

TEST(verified_dsp_speaker_protection_nan_and_inf_clamp)
{
    // Even if a user kernel explicitly divides by 0.0, computes 0.0 / 0.0 (NaN),
    // sqrt(-1.0) (NaN), or multiplies by 1e20 (+Inf), the hardware speaker-protection
    // epilogue is SMT-proven to sanitize NaN -> 0.0f and clamp +-Inf to [-maxAmp, +maxAmp].
    VerifiedDspCompileOptions opts;
    opts.maxAmp = 0.8f;

    auto divZeroKernel = compileVerifiedDspKernel("(sample) => sample / 0.0", opts);
    ASSERT_TRUE(divZeroKernel.valid);
    ASSERT_TRUE(divZeroKernel.verified);
    ASSERT_TRUE(divZeroKernel.nanSafeProven);

    // Positive sample / 0.0 -> +Inf -> clamped to +maxAmp (0.8f)
    float posInfClamped = divZeroKernel.eval(1.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(std::isfinite(posInfClamped));
    ASSERT_NEAR(posInfClamped, 0.8f, 1e-6f);

    // Negative sample / 0.0 -> -Inf -> clamped to -maxAmp (-0.8f)
    float negInfClamped = divZeroKernel.eval(-1.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(std::isfinite(negInfClamped));
    ASSERT_NEAR(negInfClamped, -0.8f, 1e-6f);

    // 0.0 / 0.0 -> NaN -> sanitized to 0.0f
    float nanSanitized = divZeroKernel.eval(0.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(std::isfinite(nanSanitized));
    ASSERT_NEAR(nanSanitized, 0.0f, 1e-6f);

    // Explicit NaN input sample -> sanitized to 0.0f
    float nanIn = std::numeric_limits<float>::quiet_NaN();
    float nanInOut = divZeroKernel.eval(nanIn, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(std::isfinite(nanInOut));
    ASSERT_NEAR(nanInOut, 0.0f, 1e-6f);

    // sqrt(-4.0) -> NaN -> sanitized to 0.0f
    auto sqrtNegKernel = compileVerifiedDspKernel("(sample) => Math.sqrt(sample)", opts);
    ASSERT_TRUE(sqrtNegKernel.valid && sqrtNegKernel.nanSafeProven);
    float sqOut = sqrtNegKernel.eval(-4.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(std::isfinite(sqOut));
    ASSERT_NEAR(sqOut, 0.0f, 1e-6f);

    PASS();
}

TEST(verified_dsp_rejects_unsafe_js_constructs)
{
    const char* unsafeSnippets[] = {
        "(sample) => { const a = new Float32Array(16); return sample; }",
        "(sample) => { const obj = { x: sample }; return sample; }",
        "(sample) => { const arr = [sample, 1.0]; return sample; }",
        "(sample) => { while (sample > 0.0) { sample = sample - 0.1; } return sample; }",
        "(sample) => { for (let i = 0; i < 10; i = i + 1) {} return sample; }",
        "(sample) => { throw sample; }",
        "(sample) => { try { return sample; } catch (e) { return 0.0; } }",
        "(sample) => { const s = \"alloc\"; return sample; }",
        "(sample) => { const fn = (x) => x * 2.0; return sample; }",
        "(sample) => globalThis.sample",
        "(sample) => sample.foo",
    };

    for (const char* bad : unsafeSnippets) {
        auto res = compileVerifiedDspKernel(bad);
        if (res.valid || res.verified) {
            std::printf("  Unexpectedly accepted unsafe snippet: %s\n", bad);
        }
        ASSERT_TRUE(!res.valid);
        ASSERT_TRUE(!res.verified);
        ASSERT_TRUE(!res.zeroAllocProven);
        ASSERT_TRUE(!res.errorMessage.empty());
    }
    PASS();
}

TEST(verified_dsp_lock_free_hot_swap_and_param_modulation)
{
    CustomJitDspNode node;

    // Install initial kernel: y = sample * p0 + p1
    ASSERT_TRUE(node.compileAndInstall("(sample, time, p0, p1) => sample * p0 + p1"));
    ASSERT_TRUE(node.isVerified());
    ASSERT_TRUE(node.isZeroAllocProven());
    ASSERT_TRUE(node.isNanSafeProven());
    ASSERT_TRUE(node.isMirEquivProven());
    ASSERT_EQ(node.hotSwapCount(), 1u);

    node.setParam(0, 0.5f);
    node.setParam(1, 0.25f);

    constexpr uint32_t kBlockSize = 128;
    std::vector<float> inBuf(kBlockSize, 0.4f);
    std::vector<float> outBuf(kBlockSize, 0.0f);
    node.processBlock(inBuf.data(), outBuf.data(), kBlockSize, 0.0, 48000.0);
    for (uint32_t i = 0; i < kBlockSize; ++i) {
        ASSERT_NEAR(outBuf[i], 0.4f * 0.5f + 0.25f, 1e-5f);
    }

    // Attempt hot-swap with an invalid kernel (contains heap allocation):
    // must be rejected and leave the existing kernel running uninterrupted!
    std::string err;
    ASSERT_TRUE(!node.hotSwap("(sample) => { const b = new Array(4); return sample; }", {}, &err));
    ASSERT_TRUE(!err.empty());
    ASSERT_EQ(node.hotSwapCount(), 1u);
    ASSERT_NEAR(node.evalSample(0.4f, 0.0f), 0.45f, 1e-5f);

    // Concurrent audio-thread rendering while main thread hot-swaps kernels & modulates p0/p1
    std::atomic<bool> stopAudio{false};
    std::atomic<uint64_t> blocksRendered{0};
    std::atomic<bool> sawNonFiniteOrOutOfBounds{false};

    std::thread audioThread([&]() {
        float blockIn[64];
        float blockOut[64];
        for (int i = 0; i < 64; ++i) blockIn[i] = (i % 2 == 0) ? 0.6f : -0.6f;
        double t = 0.0;
        while (!stopAudio.load(std::memory_order_relaxed)) {
            node.processBlock(blockIn, blockOut, 64, t, 48000.0);
            for (int i = 0; i < 64; ++i) {
                if (!std::isfinite(blockOut[i]) || blockOut[i] < -1.0f || blockOut[i] > 1.0f) {
                    sawNonFiniteOrOutOfBounds.store(true, std::memory_order_relaxed);
                }
            }
            t += 64.0 / 48000.0;
            blocksRendered.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const char* liveKernels[] = {
        "(sample, time, drive, mix) => Math.tanh(sample * drive) * mix",
        "(sample, time, freq, amp) => Math.sin(time * freq * 6.2831853) * amp",
        "(sample, time, lo, hi) => clamp(sample * 2.0, lo, hi)",
        "(sample) => sample / 0.0", // Deliberate div-by-zero kernel to test live speaker protection
    };

    for (int iter = 0; iter < 8; ++iter) {
        node.setParam(0, 1.5f + 0.1f * static_cast<float>(iter));
        node.setParam(1, 0.7f);
        ASSERT_TRUE(node.hotSwap(liveKernels[iter % 4]));
    }

    while (blocksRendered.load(std::memory_order_relaxed) < 20) {
        std::this_thread::yield();
    }
    stopAudio.store(true, std::memory_order_relaxed);
    audioThread.join();

    ASSERT_TRUE(!sawNonFiniteOrOutOfBounds.load());
    ASSERT_EQ(node.hotSwapCount(), 9u);
    PASS();
}

TEST(verified_dsp_engine_master_bus_integration)
{
    Engine engine;
    ASSERT_TRUE(engine.initHeadless());
    engine.setMasterGain(1.0f);
    // Warm up smoothMasterGain_ so it reaches 1.0f
    engine.renderBlock(512);

    auto dsp = std::make_shared<VerifiedDspProcessor>();
    // Constant synthesis kernel generating +0.35 on the master bus
    ASSERT_TRUE(dsp->compileAndInstall("(sample, time, p0) => p0"));
    dsp->setParam(0, 0.35f);

    ASSERT_TRUE(engine.masterBus() != nullptr);
    engine.masterBus()->customDspProcessor.store(dsp, std::memory_order_release);

    engine.startRecording(2, 5.0);
    engine.renderBlock(512);
    engine.stopRecording();
    ASSERT_NEAR(engine.masterBus()->peakL.load(), 0.35f, 1e-4f);
    ASSERT_NEAR(engine.masterBus()->peakR.load(), 0.35f, 1e-4f);
    std::vector<float> stereoOut = engine.getRecordBuffer();
    ASSERT_TRUE(stereoOut.size() >= 512 * 2);

    // After masterLimiter_'s 5ms (~220 frames) lookahead delay, output is +0.35
    for (int i = 300 * 2; i < 512 * 2; ++i) {
        ASSERT_NEAR(stereoOut[i], 0.35f, 1e-3f);
    }

    // Live hot-swap to -0.25 on the master bus
    ASSERT_TRUE(dsp->hotSwap("(sample, time, p0) => -0.25"));
    engine.startRecording(2, 5.0);
    engine.renderBlock(512);
    engine.stopRecording();
    ASSERT_NEAR(engine.masterBus()->peakL.load(), 0.25f, 1e-4f);
    ASSERT_NEAR(engine.masterBus()->peakR.load(), 0.25f, 1e-4f);
    stereoOut = engine.getRecordBuffer();
    ASSERT_TRUE(stereoOut.size() >= 512 * 2);
    for (int i = 300 * 2; i < 512 * 2; ++i) {
        ASSERT_NEAR(stereoOut[i], -0.25f, 1e-3f);
    }

    engine.shutdown();
    PASS();
}

#endif // BROAUDIO_HAS_JIT

int main() {
    return runAllTests();
}
