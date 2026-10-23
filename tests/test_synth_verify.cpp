#include "test_harness.h"
#include "synth_test_graphs.h"
#include "broaudio/synth/synth_verify.h"
#include "broaudio/dsp/jit/jit_compiler.h"
#include "broaudio/dsp/jit/jit_bus_pipeline.h"
#include "broaudio/dsp/biquad.h"
#include "broaudio/dsp/distortion.h"

#if BROAUDIO_HAS_JIT
#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/types.hpp>
#endif

#include <cmath>
#include <limits>
#include <vector>

using namespace broaudio;

#if BROAUDIO_HAS_JIT

TEST(synth_graph_mir_equivalence_all_shapes)
{
    const char* graphs[] = {
        synthtest::kGunshot,
        synthtest::kFmBell,
        synthtest::kMetalHit,
        synthtest::kKitchenSink,
    };
    for (const char* json : graphs) {
        auto g = SynthGraph::fromJson(json);
        ASSERT_TRUE(g != nullptr);
        SynthMirVerifyResult res = verify_synth_graph_mir_equivalence(*g, 2);
        if (!res.proven) {
            std::printf("  MIR equiv failed: %s\n", res.diagnostic.c_str());
        }
        ASSERT_TRUE(res.proven);
    }

    // Impulse generator shapes across all 4 variants (Impulse, Rect, Hann, Decay)
    const char* impulse_json = R"({"nodes": {
        "i1": {"type": "impulses", "shape": "impulse", "rate": 12, "jitter": 0.4, "ampJitter": 0.3},
        "i2": {"type": "impulses", "shape": "rect", "rate": 18, "length": 0.005},
        "i3": {"type": "impulses", "shape": "hann", "rate": 24, "length": 0.008},
        "i4": {"type": "impulses", "shape": "decay", "rate": 30, "length": 0.012},
        "m":  {"type": "mix", "inputs": ["i1", "i2", "i3", "i4"]}
    }, "output": "m"})";
    auto imp_g = SynthGraph::fromJson(impulse_json);
    SynthMirVerifyResult imp_res = verify_synth_graph_mir_equivalence(*imp_g, 2);
    ASSERT_TRUE(imp_res.proven);
    PASS();
}

TEST(bus_kernel_mir_equivalence_all_topologies)
{
    const DistortionMode modes[] = {
        DistortionMode::SoftClip,
        DistortionMode::HardClip,
        DistortionMode::Foldback,
        DistortionMode::Bitcrush,
    };
    for (int filters : {0, 1, 2, 4}) {
        for (bool has_dist : {false, true}) {
            for (bool fuse_gp : {false, true}) {
                for (DistortionMode mode : modes) {
                    if (!has_dist && mode != DistortionMode::SoftClip) continue;
                    JitTopology topo{};
                    topo.filterCount = filters;
                    topo.hasDistortion = has_dist;
                    topo.distortionMode = mode;
                    topo.fuseGainPan = fuse_gp;

                    SynthMirVerifyResult res = verify_bus_kernel_mir_equivalence(topo, 2);
                    if (!res.proven) {
                        std::printf("  Bus MIR equiv failed (filters=%d, dist=%d, mode=%d, gp=%d): %s\n",
                                    filters, (int)has_dist, (int)mode, (int)fuse_gp,
                                    res.diagnostic.c_str());
                    }
                    ASSERT_TRUE(res.proven);
                }
            }
        }
    }
    PASS();
}

TEST(voice_kernel_mir_equivalence_all_topologies)
{
    for (bool stereo : {false, true}) {
        for (auto delay : {VoiceJitTopology::Delay::None,
                           VoiceJitTopology::Delay::Onset,
                           VoiceJitTopology::Delay::Interpolated}) {
            for (bool head : {false, true}) {
                for (bool send : {false, true}) {
                    VoiceJitTopology topo{};
                    topo.stereo = stereo;
                    topo.delay = delay;
                    topo.head = head;
                    topo.send = send;

                    SynthMirVerifyResult res = verify_voice_kernel_mir_equivalence(topo, 2);
                    if (!res.proven) {
                        std::printf("  Voice MIR equiv failed: %s\n", res.diagnostic.c_str());
                    }
                    ASSERT_TRUE(res.proven);
                }
            }
        }
    }
    PASS();
}

TEST(synth_nan_and_div_by_zero_safety_proofs)
{
    const char* graphs[] = {
        synthtest::kGunshot,
        synthtest::kFmBell,
        synthtest::kMetalHit,
        synthtest::kKitchenSink,
    };
    for (const char* json : graphs) {
        auto g = SynthGraph::fromJson(json);
        ASSERT_TRUE(g != nullptr);
        SynthNanSafetyResult safety = verify_synth_nan_safety(*g);
        if (!safety.safe) {
            std::printf("  Synth NaN safety failed: %s\n", safety.diagnostic.c_str());
        }
        ASSERT_TRUE(safety.safe);
        ASSERT_EQ(safety.divisions_checked, safety.divisions_proven);
        ASSERT_EQ(safety.sqrts_checked, safety.sqrts_proven);
    }

    // Verify kKitchenSink specifically has divisions proven (oscIncrement + SVF filterCoefs + tanh shaper)
    auto ks = SynthGraph::fromJson(synthtest::kKitchenSink);
    SynthNanSafetyResult ks_safety = verify_synth_nan_safety(*ks);
    ASSERT_TRUE(ks_safety.safe);
    ASSERT_GT(ks_safety.divisions_proven, 8u);
    for (const auto& item : ks_safety.items) {
        ASSERT_TRUE(item.proven_safe);
        ASSERT_GT(item.lower_bound, 0.0f);
        ASSERT_TRUE(std::isfinite(item.upper_bound));
    }
    PASS();
}

TEST(bus_nan_and_div_by_zero_safety_proofs)
{
    for (DistortionMode mode : {DistortionMode::SoftClip,
                                DistortionMode::HardClip,
                                DistortionMode::Foldback}) {
        JitTopology topo{};
        topo.filterCount = 2;
        topo.hasDistortion = true;
        topo.distortionMode = mode;
        topo.fuseGainPan = true;

        SynthNanSafetyResult safety = verify_bus_nan_safety(topo);
        if (!safety.safe) {
            std::printf("  Bus NaN safety failed (mode=%d): %s\n",
                        (int)mode, safety.diagnostic.c_str());
        }
        ASSERT_TRUE(safety.safe);
        ASSERT_EQ(safety.divisions_checked, safety.divisions_proven);
        if (mode == DistortionMode::SoftClip) {
            // SoftClip emits 2 divisions (L and R channels): 2.0f / (1.0f + exp2_fast(...))
            ASSERT_EQ(safety.divisions_proven, 2u);
        }
    }
    PASS();
}

TEST(mir_nan_safety_catches_unguarded_hazards_and_proves_guarded)
{
    using namespace brass;

    // 1. Unguarded float division: 1.0f / x -> MUST fail safety proof
    {
        Module mod("unguarded_fdiv_mod");
        Function* fn = mod.create_function("unguarded_fdiv", Type::f32(), {Type::f32()});
        Builder b(mod);
        b.set_function(fn);
        BasicBlock* bb = b.append_block("entry");
        Value* x = b.add_block_param(bb, Type::f32());
        b.position_at_end(bb);
        Value* q = b.build_sdiv(b.build_fconst_f32(1.0f), x);
        b.build_ret(q);

        SynthNanSafetyResult r = verify_mir_nan_safety(*fn);
        ASSERT_FALSE(r.safe);
        ASSERT_EQ(r.divisions_checked, 1u);
        ASSERT_EQ(r.divisions_proven, 0u);
        ASSERT_EQ(r.items[0].hazard, SafetyHazardKind::FloatDivByZeroOrInf);
    }

    // 2. Insufficiently guarded float division: 1.0f / fmax(x, -1.0f) -> straddles 0, MUST fail
    {
        Module mod("straddling_fdiv_mod");
        Function* fn = mod.create_function("straddling_fdiv", Type::f32(), {Type::f32()});
        Builder b(mod);
        b.set_function(fn);
        BasicBlock* bb = b.append_block("entry");
        Value* x = b.add_block_param(bb, Type::f32());
        b.position_at_end(bb);
        Value* clamped = b.build_fmin_f32(b.build_fmax_f32(x, b.build_fconst_f32(-1.0f)),
                                          b.build_fconst_f32(1.0f));
        Value* q = b.build_sdiv(b.build_fconst_f32(1.0f), clamped);
        b.build_ret(q);

        SynthNanSafetyResult r = verify_mir_nan_safety(*fn);
        ASSERT_FALSE(r.safe);
        ASSERT_EQ(r.divisions_proven, 0u);
    }

    // 3. Properly guarded float division: 1.0f / fmin(fmax(x, 0.05f), 1000.0f) -> MUST pass
    {
        Module mod("guarded_fdiv_mod");
        Function* fn = mod.create_function("guarded_fdiv", Type::f32(), {Type::f32()});
        Builder b(mod);
        b.set_function(fn);
        BasicBlock* bb = b.append_block("entry");
        Value* x = b.add_block_param(bb, Type::f32());
        b.position_at_end(bb);
        Value* denom = b.build_fmin_f32(b.build_fmax_f32(x, b.build_fconst_f32(0.05f)),
                                        b.build_fconst_f32(1000.0f));
        Value* q = b.build_sdiv(b.build_fconst_f32(1.0f), denom);
        b.build_ret(q);

        SynthNanSafetyResult r = verify_mir_nan_safety(*fn);
        ASSERT_TRUE(r.safe);
        ASSERT_EQ(r.divisions_checked, 1u);
        ASSERT_EQ(r.divisions_proven, 1u);
    }

    // 4. Unguarded sqrt_f32(x) -> MUST fail safety proof
    {
        Module mod("unguarded_sqrt_mod");
        Function* fn = mod.create_function("unguarded_sqrt", Type::f32(), {Type::f32()});
        Builder b(mod);
        b.set_function(fn);
        BasicBlock* bb = b.append_block("entry");
        Value* x = b.add_block_param(bb, Type::f32());
        b.position_at_end(bb);
        Value* s = b.build_sqrt_f32(x);
        b.build_ret(s);

        SynthNanSafetyResult r = verify_mir_nan_safety(*fn);
        ASSERT_FALSE(r.safe);
        ASSERT_EQ(r.sqrts_checked, 1u);
        ASSERT_EQ(r.sqrts_proven, 0u);
        ASSERT_EQ(r.items[0].hazard, SafetyHazardKind::NegativeSqrt);
    }

    // 5. Properly guarded sqrt_f32(fmin(fmax(x, 0.0f), 100.0f)) -> MUST pass
    {
        Module mod("guarded_sqrt_mod");
        Function* fn = mod.create_function("guarded_sqrt", Type::f32(), {Type::f32()});
        Builder b(mod);
        b.set_function(fn);
        BasicBlock* bb = b.append_block("entry");
        Value* x = b.add_block_param(bb, Type::f32());
        b.position_at_end(bb);
        Value* clamped = b.build_fmin_f32(b.build_fmax_f32(x, b.build_fconst_f32(0.0f)),
                                          b.build_fconst_f32(100.0f));
        Value* s = b.build_sqrt_f32(clamped);
        b.build_ret(s);

        SynthNanSafetyResult r = verify_mir_nan_safety(*fn);
        ASSERT_TRUE(r.safe);
        ASSERT_EQ(r.sqrts_checked, 1u);
        ASSERT_EQ(r.sqrts_proven, 1u);
    }

    // 6. Non-finite constant in MIR -> MUST fail safety proof
    {
        Module mod("nan_const_mod");
        Function* fn = mod.create_function("nan_const", Type::f32(), {});
        Builder b(mod);
        b.set_function(fn);
        BasicBlock* bb = b.append_block("entry");
        b.position_at_end(bb);
        b.build_ret(b.build_fconst_f32(std::numeric_limits<float>::quiet_NaN()));

        SynthNanSafetyResult r = verify_mir_nan_safety(*fn);
        ASSERT_FALSE(r.safe);
        ASSERT_EQ(r.items[0].hazard, SafetyHazardKind::NonFiniteConstant);
    }
    PASS();
}

TEST(differential_interp_vs_jit_parity_extreme_sweeps)
{
    // Extreme filter cutoff (sweeping from near 0 Hz up past Nyquist) and extreme resonance Q,
    // combined with LFO modulation, FM feedback, and all oscillator waveforms.
    const char* extreme_graph_json = R"({"nodes": {
        "lfo":   {"type": "osc", "wave": "sine", "freq": 23.0, "gain": 25000.0},
        "cut":   {"type": "mix", "inputs": [12000.0, "lfo"]},
        "qlfo":  {"type": "osc", "wave": "triangle", "freq": 17.0, "gain": 80.0},
        "qmod":  {"type": "mix", "inputs": [40.0, "qlfo"]},
        "fm":    {"type": "fm", "freq": "cut", "ratio": 2.718, "index": 12.0, "feedback": 1.5},
        "saw":   {"type": "osc", "wave": "saw", "freq": "cut"},
        "sq":    {"type": "osc", "wave": "square", "freq": 440.0, "pw": "qlfo"},
        "src":   {"type": "mix", "inputs": ["fm", "saw", "sq"]},
        "lp":    {"type": "filter", "mode": "lowpass", "input": "src", "cutoff": "cut", "q": "qmod"},
        "hp":    {"type": "filter", "mode": "highpass", "input": "lp", "cutoff": "cut", "q": "qmod"},
        "bp":    {"type": "filter", "mode": "bandpass", "input": "hp", "cutoff": "cut", "q": "qmod"},
        "notch": {"type": "filter", "mode": "notch", "input": "bp", "cutoff": "cut", "q": "qmod"},
        "tanh":  {"type": "shaper", "mode": "tanh", "input": "notch", "drive": 8.0},
        "fold":  {"type": "shaper", "mode": "fold", "input": "tanh", "drive": 4.0},
        "env":   {"type": "env", "attack": 0.002, "decay": 0.05, "sustain": 0.4, "release": 0.02, "gate": 0.06},
        "out":   {"type": "mul", "a": "fold", "b": "env"}
    }, "output": "out"})";

    auto g = SynthGraph::fromJson(extreme_graph_json);
    ASSERT_TRUE(g != nullptr);

    // Prove both MIR equivalence and NaN/div-by-zero safety on this extreme graph
    SynthMirVerifyResult eq_res = verify_synth_graph_mir_equivalence(*g, 2);
    ASSERT_TRUE(eq_res.proven);

    SynthNanSafetyResult nan_res = verify_synth_nan_safety(*g);
    ASSERT_TRUE(nan_res.safe);
    ASSERT_GT(nan_res.divisions_proven, 10u);

    // Run differential parity across multiple seeds and sample rates
    for (int sr : {22050, 44100, 48000, 96000}) {
        for (uint32_t seed : {0u, 1u, 42u, 99999u}) {
            SynthTrigger trig{};
            trig.seed = seed;
            trig.jitter = true;
            SynthParityResult parity = verify_synth_parity(g, trig, sr, 2048);
            if (!parity.passed) {
                std::printf("  Parity failed (sr=%d, seed=%u): %s\n",
                            sr, seed, parity.diagnostic.c_str());
            }
            ASSERT_TRUE(parity.passed);
            ASSERT_FALSE(parity.any_nan_or_inf);
        }
    }
    PASS();
}

TEST(bus_pipeline_nan_sanitization_and_foldback_parity)
{
    JitTopology topo{};
    topo.filterCount = 1;
    topo.hasDistortion = true;
    topo.distortionMode = DistortionMode::Foldback;
    topo.fuseGainPan = false;

    JitCompiler compiler;
    auto pipeline = compiler.compileSync(topo);
    ASSERT_TRUE(pipeline != nullptr);

    BiquadFilter refFilter;
    refFilter.type = BiquadFilter::Type::Lowpass;
    refFilter.frequency = 1500.0f;
    refFilter.Q = 2.0f;
    refFilter.computeCoefficients(48000);
    refFilter.snapToTarget();

    Distortion refDist;
    refDist.enabled = true;
    refDist.mode = DistortionMode::Foldback;
    refDist.drive = 3.5f;
    refDist.mix = 0.8f;
    refDist.outputGain = 0.9f;

    pipeline->params().activeFilterCount = 1;
    pipeline->params().filters[0] = {refFilter.b0, refFilter.b1, refFilter.b2, refFilter.a1, refFilter.a2};
    pipeline->params().distortionDrive = refDist.drive;
    pipeline->params().distortionMix = refDist.mix;
    pipeline->params().distortionOutputGain = refDist.outputGain;

    constexpr int kFrames = 128;
    std::vector<float> jitBuf(kFrames * 2);
    std::vector<float> refBuf(kFrames * 2);
    for (int i = 0; i < kFrames; ++i) {
        float sL = 0.8f * std::sin(0.1f * static_cast<float>(i));
        float sR = 0.8f * std::cos(0.13f * static_cast<float>(i));
        jitBuf[i * 2 + 0] = refBuf[i * 2 + 0] = sL;
        jitBuf[i * 2 + 1] = refBuf[i * 2 + 1] = sR;
    }

    pipeline->process(jitBuf.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        refBuf[i * 2 + 0] = refFilter.process(refBuf[i * 2 + 0], 0);
        refBuf[i * 2 + 1] = refFilter.process(refBuf[i * 2 + 1], 1);
    }
    refDist.processStereo(refBuf.data(), kFrames);

    for (int i = 0; i < kFrames * 2; ++i) {
        ASSERT_TRUE(std::isfinite(jitBuf[i]));
        ASSERT_NEAR(jitBuf[i], refBuf[i], 1e-4f);
    }

    // Inject NaN and +-Inf into input buffer and verify JitBusPipeline sanitizes them
    jitBuf[0] = std::numeric_limits<float>::quiet_NaN();
    jitBuf[1] = std::numeric_limits<float>::infinity();
    jitBuf[2] = -std::numeric_limits<float>::infinity();
    pipeline->process(jitBuf.data(), kFrames);
    for (int i = 0; i < kFrames * 2; ++i) {
        ASSERT_TRUE(std::isfinite(jitBuf[i]));
    }
    ASSERT_TRUE(std::isfinite(pipeline->state().filterStateL[0].z1));
    ASSERT_TRUE(std::isfinite(pipeline->state().filterStateL[0].z2));
    ASSERT_TRUE(std::isfinite(pipeline->state().filterStateR[0].z1));
    ASSERT_TRUE(std::isfinite(pipeline->state().filterStateR[0].z2));
    PASS();
}

#endif // BROAUDIO_HAS_JIT

int main()
{
    std::printf("=== test_synth_verify ===\n");
    return runAllTests();
}
