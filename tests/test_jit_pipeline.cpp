#include "test_harness.h"
#include "broaudio/engine.h"
#include "broaudio/dsp/jit/jit_compiler.h"
#include "broaudio/dsp/jit/jit_bus_pipeline.h"
#include "broaudio/dsp/biquad.h"
#include "broaudio/dsp/distortion.h"

#include <cmath>
#include <vector>
#include <thread>
#include <chrono>

using namespace broaudio;

TEST(biquad_single_filter_equivalence) {
    int sampleRate = 48000;
    int numFrames = 256;

    BiquadFilter refFilter;
    refFilter.type = BiquadFilter::Type::Lowpass;
    refFilter.frequency = 1200.0f;
    refFilter.Q = 1.414f;
    refFilter.computeCoefficients(sampleRate);
    refFilter.snapToTarget();

    JitTopology topo;
    topo.filterCount = 1;
    topo.hasDistortion = false;

    JitCompiler compiler;
    auto pipeline = compiler.compileSync(topo);
    ASSERT_TRUE(pipeline != nullptr);
    ASSERT_TRUE(pipeline->functionPointer() != nullptr);

    // Setup input buffer with multi-frequency test tone
    std::vector<float> jitBuf(numFrames * 2);
    std::vector<float> refBuf(numFrames * 2);
    for (int i = 0; i < numFrames; i++) {
        float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        float sL = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * t) +
                   0.3f * std::sin(2.0f * 3.14159265f * 3500.0f * t);
        float sR = 0.4f * std::cos(2.0f * 3.14159265f * 880.0f * t) +
                   0.2f * std::cos(2.0f * 3.14159265f * 5000.0f * t);
        jitBuf[i * 2 + 0] = refBuf[i * 2 + 0] = sL;
        jitBuf[i * 2 + 1] = refBuf[i * 2 + 1] = sR;
    }

    // Configure pipeline params
    pipeline->params().activeFilterCount = 1;
    pipeline->params().filters[0].b0 = refFilter.b0;
    pipeline->params().filters[0].b1 = refFilter.b1;
    pipeline->params().filters[0].b2 = refFilter.b2;
    pipeline->params().filters[0].a1 = refFilter.a1;
    pipeline->params().filters[0].a2 = refFilter.a2;

    // Process via JIT
    pipeline->process(jitBuf.data(), numFrames);

    // Process via reference scalar biquad
    for (int i = 0; i < numFrames; i++) {
        refBuf[i * 2 + 0] = refFilter.process(refBuf[i * 2 + 0], 0);
        refBuf[i * 2 + 1] = refFilter.process(refBuf[i * 2 + 1], 1);
    }

    // Compare results
    for (int i = 0; i < numFrames * 2; i++) {
        ASSERT_NEAR(jitBuf[i], refBuf[i], 1e-4f);
    }

    // Verify filter state history
    ASSERT_NEAR(pipeline->state().filterStateL[0].z1, refFilter.z1[0], 1e-4f);
    ASSERT_NEAR(pipeline->state().filterStateL[0].z2, refFilter.z2[0], 1e-4f);
    ASSERT_NEAR(pipeline->state().filterStateR[0].z1, refFilter.z1[1], 1e-4f);
    ASSERT_NEAR(pipeline->state().filterStateR[0].z2, refFilter.z2[1], 1e-4f);
    g_testsPassed++;
}

TEST(distortion_softclip_equivalence) {
    int numFrames = 256;

    JitTopology topo;
    topo.filterCount = 0;
    topo.hasDistortion = true;
    topo.distortionMode = DistortionMode::SoftClip;

    JitCompiler compiler;
    auto pipeline = compiler.compileSync(topo);
    ASSERT_TRUE(pipeline != nullptr);

    Distortion refDist;
    refDist.enabled = true;
    refDist.mode = DistortionMode::SoftClip;
    refDist.drive = 2.5f;
    refDist.mix = 0.75f;
    refDist.outputGain = 0.85f;

    pipeline->params().distortionDrive = refDist.drive;
    pipeline->params().distortionMix = refDist.mix;
    pipeline->params().distortionOutputGain = refDist.outputGain;

    std::vector<float> jitBuf(numFrames * 2);
    std::vector<float> refBuf(numFrames * 2);
    for (int i = 0; i < numFrames; i++) {
        float xL = (static_cast<float>(i) / static_cast<float>(numFrames)) * 2.0f - 1.0f;
        float xR = -xL;
        jitBuf[i * 2 + 0] = refBuf[i * 2 + 0] = xL;
        jitBuf[i * 2 + 1] = refBuf[i * 2 + 1] = xR;
    }

    pipeline->process(jitBuf.data(), numFrames);
    refDist.processStereo(refBuf.data(), numFrames);

    for (int i = 0; i < numFrames * 2; i++) {
        ASSERT_NEAR(jitBuf[i], refBuf[i], 1e-3f);
    }
    g_testsPassed++;
}

TEST(fused_cascade_state_persistence) {
    int sampleRate = 44100;
    int numFrames = 128;

    BiquadFilter f1, f2;
    f1.type = BiquadFilter::Type::Lowpass;
    f1.frequency = 900.0f;
    f1.Q = 1.0f;
    f1.computeCoefficients(sampleRate);
    f1.snapToTarget();

    f2.type = BiquadFilter::Type::Peaking;
    f2.frequency = 2500.0f;
    f2.Q = 2.0f;
    f2.gainDB = 6.0f;
    f2.computeCoefficients(sampleRate);
    f2.snapToTarget();

    Distortion dist;
    dist.enabled = true;
    dist.mode = DistortionMode::SoftClip;
    dist.drive = 1.8f;
    dist.mix = 0.6f;
    dist.outputGain = 0.95f;

    JitTopology topo;
    topo.filterCount = 2;
    topo.hasDistortion = true;
    topo.distortionMode = DistortionMode::SoftClip;

    JitCompiler compiler;
    auto pipeline = compiler.compileSync(topo);

    pipeline->params().activeFilterCount = 2;
    pipeline->params().filters[0].b0 = f1.b0;
    pipeline->params().filters[0].b1 = f1.b1;
    pipeline->params().filters[0].b2 = f1.b2;
    pipeline->params().filters[0].a1 = f1.a1;
    pipeline->params().filters[0].a2 = f1.a2;

    pipeline->params().filters[1].b0 = f2.b0;
    pipeline->params().filters[1].b1 = f2.b1;
    pipeline->params().filters[1].b2 = f2.b2;
    pipeline->params().filters[1].a1 = f2.a1;
    pipeline->params().filters[1].a2 = f2.a2;

    pipeline->params().distortionDrive = dist.drive;
    pipeline->params().distortionMix = dist.mix;
    pipeline->params().distortionOutputGain = dist.outputGain;

    // Process 4 consecutive blocks to verify state preservation across block boundaries
    for (int block = 0; block < 4; block++) {
        std::vector<float> jitBuf(numFrames * 2);
        std::vector<float> refBuf(numFrames * 2);
        for (int i = 0; i < numFrames; i++) {
            float t = static_cast<float>(block * numFrames + i) / static_cast<float>(sampleRate);
            float v = std::sin(2.0f * 3.14159265f * 600.0f * t);
            jitBuf[i * 2 + 0] = refBuf[i * 2 + 0] = v;
            jitBuf[i * 2 + 1] = refBuf[i * 2 + 1] = v * 0.8f;
        }

        pipeline->process(jitBuf.data(), numFrames);

        for (int i = 0; i < numFrames; i++) {
            refBuf[i * 2 + 0] = f1.process(refBuf[i * 2 + 0], 0);
            refBuf[i * 2 + 1] = f1.process(refBuf[i * 2 + 1], 1);
            refBuf[i * 2 + 0] = f2.process(refBuf[i * 2 + 0], 0);
            refBuf[i * 2 + 1] = f2.process(refBuf[i * 2 + 1], 1);
        }
        dist.processStereo(refBuf.data(), numFrames);

        for (int i = 0; i < numFrames * 2; i++) {
            ASSERT_NEAR(jitBuf[i], refBuf[i], 1e-2f);
        }
    }
    g_testsPassed++;
}

TEST(jit_compiler_cache) {
    JitCompiler compiler;
    ASSERT_EQ(compiler.cacheSize(), 0);

    JitTopology t1;
    t1.filterCount = 1;
    t1.hasDistortion = false;

    auto p1 = compiler.compileSync(t1);
    ASSERT_TRUE(p1 != nullptr);
    ASSERT_EQ(compiler.cacheSize(), 1);
    ASSERT_TRUE(compiler.isCached(t1));

    // Second call with same topology should hit cache
    auto p2 = compiler.compileSync(t1);
    ASSERT_TRUE(p2 != nullptr);
    ASSERT_EQ(compiler.cacheSize(), 1);

    // Different topology
    JitTopology t2;
    t2.filterCount = 2;
    t2.hasDistortion = true;
    t2.distortionMode = DistortionMode::HardClip;

    auto p3 = compiler.compileSync(t2);
    ASSERT_TRUE(p3 != nullptr);
    ASSERT_EQ(compiler.cacheSize(), 2);
    ASSERT_TRUE(compiler.isCached(t2));

    compiler.clearCache();
    ASSERT_EQ(compiler.cacheSize(), 0);
    g_testsPassed++;
}

TEST(engine_bus_jit_activation_and_fallback) {
    Engine engine;
    engine.init();

    int busId = engine.createBus();
    ASSERT_TRUE(engine.isBusJitEnabled(busId));

    // Configure a biquad filter on the bus
    engine.setBusFilterEnabled(busId, 0, true);
    engine.setBusFilterType(busId, 0, BiquadFilter::Type::Lowpass);
    engine.setBusFilterFrequency(busId, 0, 1500.0f);
    engine.setBusFilterQ(busId, 0, 1.2f);

    // Give background compilation thread a brief moment to compile and publish
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Render a block
    engine.renderBlock(128);

    // JIT should be active for this bus
    ASSERT_TRUE(engine.isBusJitActive(busId));

    // Enable an unsupported effect on the bus (Delay)
    engine.setBusDelayEnabled(busId, true);
    engine.setBusDelayTime(busId, 0.2f);
    engine.setBusDelayFeedback(busId, 0.3f);
    engine.setBusDelayMix(busId, 0.4f);

    // Render block: should cleanly fall back to sequential OOP pipeline
    engine.renderBlock(128);
    ASSERT_FALSE(engine.isBusJitActive(busId));

    // Disable Delay again: should restore fast JIT path
    engine.setBusDelayEnabled(busId, false);
    engine.renderBlock(128);
    ASSERT_TRUE(engine.isBusJitActive(busId));

    // Disable JIT explicitly on the bus
    engine.setBusJitEnabled(busId, false);
    ASSERT_FALSE(engine.isBusJitEnabled(busId));
    engine.renderBlock(128);
    ASSERT_FALSE(engine.isBusJitActive(busId));

    engine.shutdown();
    g_testsPassed++;
}

int main() {
    return runAllTests();
}
