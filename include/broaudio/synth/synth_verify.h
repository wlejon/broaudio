#pragma once

// Formal verification & differential parity suite for broaudio's SynthGraph,
// DSP Bus JIT (JitBusPipeline), and Spatial Voice JIT kernels using brass's
// SMT/CDCL bitvector equivalence verifier (brass::verify::verify_mir_equivalence),
// SMT guard elimination (brass::run_smt_guard_elimination), and MIR superoptimizer
// (brass::run_mir_superopt).

#include "broaudio/dsp/jit/jit_types.h"
#include "broaudio/spatial/voice_jit.h"
#include "broaudio/synth/synth_graph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace brass {
class Function;
class Module;
} // namespace brass

namespace broaudio {

struct LayerPlan;
struct SynthShape;

// Result of optimizing a MIR function with brass::run_smt_guard_elimination +
// brass::run_mir_superopt and proving Unopt MIR == Superopt MIR via
// brass::verify::verify_mir_equivalence.
struct SynthMirVerifyResult {
    bool proven = false;
    uint32_t guards_eliminated = 0;
    uint32_t superopt_rewrites = 0;
    std::string diagnostic;
};

// Hazard categories checked by the NaN / Div-by-Zero / Sqrt-Domain safety prover.
enum class SafetyHazardKind : uint8_t {
    None = 0,
    FloatDivByZeroOrInf,
    IntDivByZero,
    NegativeSqrt,
    NonFiniteConstant,
    UnboundedDenominator,
};

// Per-instruction proof obligation and discharge status in verify_mir_nan_safety.
struct SafetyProofItem {
    uint32_t inst_value_id = 0;
    std::string op_name;
    SafetyHazardKind hazard = SafetyHazardKind::None;
    bool proven_safe = false;
    float lower_bound = 0.0f;
    float upper_bound = 0.0f;
    std::string diagnostic;
};

// Aggregate result of verifying that a MIR function or SynthShape/JitTopology
// cannot produce NaN, Inf, division-by-zero, or negative square roots.
struct SynthNanSafetyResult {
    bool safe = false;
    uint32_t divisions_checked = 0;
    uint32_t sqrts_checked = 0;
    uint32_t divisions_proven = 0;
    uint32_t sqrts_proven = 0;
    std::vector<SafetyProofItem> items;
    std::string diagnostic;
};

// Result of running differential parity verification between the C++ reference
// interpreter (synth_interp.cpp) and the brass-compiled JIT kernel (synth_jit.cpp).
struct SynthParityResult {
    bool passed = false;
    size_t total_samples = 0;
    size_t first_mismatch_index = 0;
    float max_abs_diff = 0.0f;
    bool any_nan_or_inf = false;
    std::string diagnostic;
};

// Lower any Opcode::fma_f32 / Opcode::fma_f64 instructions in `fn` to explicit
// mul + add instructions so brass::verify::verify_mir_equivalence can symbolically
// execute the function.
void lower_fma_for_verification(brass::Function& fn, brass::Module& mod);

// Clone `fn`, run brass::run_smt_guard_elimination and brass::run_mir_superopt,
// and formally prove Unopt MIR == Superopt MIR with brass::verify::verify_mir_equivalence.
SynthMirVerifyResult optimize_and_verify_mir(brass::Function& fn,
                                             brass::Module& mod,
                                             uint32_t loop_unroll_bound = 2);

// Prove Unopt MIR == Superopt MIR for a SynthShape's layer kernel.
SynthMirVerifyResult verify_synth_shape_mir_equivalence(const SynthShape& shape,
                                                        uint32_t loop_unroll_bound = 2);

// Prove Unopt MIR == Superopt MIR for a LayerPlan's layer kernel.
SynthMirVerifyResult verify_synth_plan_mir_equivalence(const LayerPlan& plan,
                                                       uint32_t loop_unroll_bound = 2);

// Prove Unopt MIR == Superopt MIR across every layer of a SynthGraph.
SynthMirVerifyResult verify_synth_graph_mir_equivalence(const SynthGraph& graph,
                                                        uint32_t loop_unroll_bound = 2);

// Prove Unopt MIR == Superopt MIR for a fused DSP bus kernel (JitBusPipeline).
SynthMirVerifyResult verify_bus_kernel_mir_equivalence(const JitTopology& topology,
                                                       uint32_t loop_unroll_bound = 2);

// Prove Unopt MIR == Superopt MIR for a spatial voice chain kernel.
SynthMirVerifyResult verify_voice_kernel_mir_equivalence(const VoiceJitTopology& topology,
                                                         uint32_t loop_unroll_bound = 2);

// Prove that every FDiv / IDiv / FSqrt and constant in a MIR function is guarded
// against division by zero, +-Inf denominators, NaN constants, and negative sqrt
// inputs using IEEE-754 interval analysis + brass::verify::verify_mir_equivalence.
SynthNanSafetyResult verify_mir_nan_safety(const brass::Function& fn);

// Prove NaN / div-by-zero / sqrt safety for a SynthShape's layer kernel.
SynthNanSafetyResult verify_synth_nan_safety(const SynthShape& shape);

// Prove NaN / div-by-zero / sqrt safety for a LayerPlan's layer kernel.
SynthNanSafetyResult verify_synth_nan_safety(const LayerPlan& plan);

// Prove NaN / div-by-zero / sqrt safety across every layer of a SynthGraph.
SynthNanSafetyResult verify_synth_nan_safety(const SynthGraph& graph);

// Prove NaN / div-by-zero / sqrt safety for a fused DSP bus kernel topology.
SynthNanSafetyResult verify_bus_nan_safety(const JitTopology& topology);

// Prove NaN / div-by-zero / sqrt safety for a spatial voice chain kernel topology.
SynthNanSafetyResult verify_voice_nan_safety(const VoiceJitTopology& topology);

// Verify differential parity (bit-exact on x86_64, <= 1e-5f tolerance on aarch64)
// and NaN/Inf freedom between the SynthGraph interpreter and JIT kernel across
// multiple block-splitting patterns.
SynthParityResult verify_synth_parity(const std::shared_ptr<const SynthGraph>& graph,
                                      const SynthTrigger& trigger = {},
                                      int sample_rate = 48000,
                                      int num_frames = 2048);

} // namespace broaudio
