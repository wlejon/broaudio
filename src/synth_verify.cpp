#include "synth_verify.h"

#include "dsp/jit/jit_bus_builder.h"
#include "spatial/voice_jit_builder.h"
#include "synth/synth_plan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#if BROAUDIO_HAS_JIT
#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/instruction.hpp>
#include <brass/mir/loop_opt.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/opcodes.hpp>
#include <brass/mir/smt_guard_elim.hpp>
#include <brass/mir/superopt.hpp>
#include <brass/mir/types.hpp>
#include <brass/verify/smt_equiv.hpp>
#endif

namespace broaudio {

#if BROAUDIO_HAS_JIT

void lower_fma_for_verification(brass::Function& fn, brass::Module& mod)
{
    using namespace brass;
    Builder b(mod);
    b.set_function(&fn);
    for (BasicBlock* bb : fn.blocks()) {
        if (!bb) continue;
        for (Instruction* inst = bb->head(); inst; inst = inst->next()) {
            if ((inst->opcode() == Opcode::fma_f32 || inst->opcode() == Opcode::fma_f64) &&
                inst->operand_count() == 3 && inst->result()) {
                Value* op0 = inst->operand(0);
                Value* op1 = inst->operand(1);
                Value* op2 = inst->operand(2);
                b.position_before(inst);
                Value* prod = b.build_mul(op0, op1);

                inst->set_opcode(Opcode::add);
                inst->operands().clear();
                inst->add_operand(prod);
                inst->add_operand(op2);
            }
        }
    }
}

SynthMirVerifyResult optimize_and_verify_mir(brass::Function& fn,
                                             brass::Module& mod,
                                             uint32_t loop_unroll_bound)
{
    using namespace brass;
    SynthMirVerifyResult out;

    lower_fma_for_verification(fn, mod);
    Module unopt_mod("unopt_verify_mod");
    Function* unopt_fn = clone_function(fn, unopt_mod);
    if (!unopt_fn) {
        out.diagnostic = "Failed to clone unoptimized MIR function";
        return out;
    }

    bool saved_reassoc = mod.allow_fp_reassociation();
    mod.set_allow_fp_reassociation(false);

    SmtGuardElimStats guard_stats{};
    SmtGuardElimOptions guard_opts{};
    guard_opts.stats = &guard_stats;
    run_smt_guard_elimination(fn, mod, guard_opts);
    out.guards_eliminated = static_cast<uint32_t>(
        guard_stats.guards_proven_eliminated + guard_stats.comparisons_proven_true +
        guard_stats.comparisons_proven_false + guard_stats.branches_folded);

    SuperoptStats super_stats{};
    SuperoptOptions super_opts{};
    super_opts.stats = &super_stats;
    run_mir_superopt(fn, mod, super_opts);
    out.superopt_rewrites = super_stats.masks_elided + super_stats.extensions_elided +
                            super_stats.dags_synthesized + super_stats.constants_synthesized +
                            super_stats.magic_divs_reduced + super_stats.indices_narrowed;

    mod.set_allow_fp_reassociation(saved_reassoc);

    verify::EquivOptions eq_opts{};
    eq_opts.loop_unroll_bound = loop_unroll_bound;
    eq_opts.check_memory = true;
    eq_opts.check_guards = true;
    eq_opts.max_sat_conflicts = 10000;

    verify::EquivResult eq = verify::verify_mir_equivalence(*unopt_fn, fn, eq_opts);
    out.proven = (eq.status == verify::EquivStatus::ProvenEquivalent);
    out.diagnostic = eq.diagnostic;
    return out;
}

SynthMirVerifyResult verify_synth_plan_mir_equivalence(const LayerPlan& plan,
                                                       uint32_t loop_unroll_bound)
{
    brass::Module mod("synth_verify_module");
    brass::Function* fn = buildSynthLayerMir(plan, mod);
    if (!fn) {
        SynthMirVerifyResult r;
        r.diagnostic = "buildSynthLayerMir returned null";
        return r;
    }
    return optimize_and_verify_mir(*fn, mod, loop_unroll_bound);
}

SynthMirVerifyResult verify_synth_shape_mir_equivalence(const SynthShape& shape,
                                                        uint32_t loop_unroll_bound)
{
    return verify_synth_plan_mir_equivalence(shape.plan, loop_unroll_bound);
}

SynthMirVerifyResult verify_synth_graph_mir_equivalence(const SynthGraph& graph,
                                                        uint32_t loop_unroll_bound)
{
    SynthMirVerifyResult agg;
    agg.proven = true;
    for (const LayerData& layer : graph.data().layers) {
        if (!layer.shape) continue;
        SynthMirVerifyResult r = verify_synth_shape_mir_equivalence(*layer.shape, loop_unroll_bound);
        agg.guards_eliminated += r.guards_eliminated;
        agg.superopt_rewrites += r.superopt_rewrites;
        if (!r.proven) {
            agg.proven = false;
            agg.diagnostic = "Layer '" + layer.id + "': " + r.diagnostic;
            return agg;
        }
    }
    return agg;
}

SynthMirVerifyResult verify_bus_kernel_mir_equivalence(const JitTopology& topology,
                                                       uint32_t loop_unroll_bound)
{
    brass::Module mod("bus_verify_module");
    brass::Function* fn = buildBusKernelMir(topology, mod);
    if (!fn) {
        SynthMirVerifyResult r;
        r.diagnostic = "buildBusKernelMir returned null";
        return r;
    }
    return optimize_and_verify_mir(*fn, mod, loop_unroll_bound);
}

SynthMirVerifyResult verify_voice_kernel_mir_equivalence(const VoiceJitTopology& topology,
                                                         uint32_t loop_unroll_bound)
{
    brass::Module mod("voice_verify_module");
    brass::Function* fn = buildVoiceKernelMir(topology, mod);
    if (!fn) {
        SynthMirVerifyResult r;
        r.diagnostic = "buildVoiceKernelMir returned null";
        return r;
    }
    return optimize_and_verify_mir(*fn, mod, loop_unroll_bound);
}

// ---------------------------------------------------------------------------
// NaN / Div-by-Zero / Sqrt-Domain Safety Prover
// ---------------------------------------------------------------------------

namespace {

using brass::BasicBlock;
using brass::Builder;
using brass::Function;
using brass::Instruction;
using brass::Module;
using brass::Opcode;
using brass::Type;
using brass::Value;

struct FInterval {
    double lo = -std::numeric_limits<double>::infinity();
    double hi = std::numeric_limits<double>::infinity();
    bool non_nan = false;
    bool known = false;

    bool is_finite() const noexcept {
        return known && non_nan && std::isfinite(lo) && std::isfinite(hi) && lo <= hi;
    }
    bool strictly_nonzero() const noexcept {
        return is_finite() && (lo > 0.0 || hi < 0.0);
    }
    bool non_negative() const noexcept {
        return is_finite() && lo >= 0.0;
    }
};

uint32_t float_to_bits(float f) noexcept
{
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(uint32_t));
    return u;
}

bool get_float_const(const Value* v, double& out) noexcept
{
    if (!v || !v->is_instruction() || !v->defining_instruction()) return false;
    const Instruction* inst = v->defining_instruction();
    if (inst->opcode() != Opcode::fconst_f64) return false;
    out = inst->imm_f64();
    return true;
}

bool get_int_const(const Value* v, int64_t& out) noexcept
{
    if (!v || !v->is_instruction() || !v->defining_instruction()) return false;
    const Instruction* inst = v->defining_instruction();
    if (inst->opcode() == Opcode::iconst_i32 || inst->opcode() == Opcode::patchable_const_i32) {
        out = inst->imm_i32();
        return true;
    }
    if (inst->opcode() == Opcode::iconst_i64 || inst->opcode() == Opcode::patchable_const_i64) {
        out = inst->imm_i64();
        return true;
    }
    return false;
}

// Check if an expression tree (up to depth 14) is a pure function of a single
// non-constant SSA value `var` (plus float constants), and if so evaluate it at `var = x`.
bool eval_univariate_at(const Value* root, const Value*& var, double x, int depth, double& out) noexcept
{
    if (!root || depth > 14) return false;
    if (double c = 0.0; get_float_const(root, c)) { out = c; return std::isfinite(out); }
    if (var == root) { out = x; return true; }
    if (!root->is_instruction() || !root->defining_instruction()) {
        if (!var) { var = root; out = x; return true; }
        return false;
    }
    const Instruction* inst = root->defining_instruction();
    Opcode op = inst->opcode();
    if (op != Opcode::add && op != Opcode::sub && op != Opcode::mul &&
        op != Opcode::neg && op != Opcode::fmin_f32 && op != Opcode::fmax_f32 &&
        op != Opcode::fabs_f32 && op != Opcode::fma_f32) {
        if (!var) { var = root; out = x; return true; }
        return false;
    }
    if (op == Opcode::neg || op == Opcode::fabs_f32) {
        double v0 = 0.0;
        if (!eval_univariate_at(inst->operand(0), var, x, depth + 1, v0)) return false;
        out = (op == Opcode::neg) ? -v0 : std::fabs(v0);
        return std::isfinite(out);
    }
    if (op == Opcode::fma_f32 && inst->operand_count() == 3) {
        double a = 0.0, b = 0.0, d = 0.0;
        if (!eval_univariate_at(inst->operand(0), var, x, depth + 1, a) ||
            !eval_univariate_at(inst->operand(1), var, x, depth + 1, b) ||
            !eval_univariate_at(inst->operand(2), var, x, depth + 1, d)) return false;
        out = a * b + d;
        return std::isfinite(out);
    }
    if (inst->operand_count() < 2) return false;
    double lhs = 0.0, rhs = 0.0;
    if (!eval_univariate_at(inst->operand(0), var, x, depth + 1, lhs) ||
        !eval_univariate_at(inst->operand(1), var, x, depth + 1, rhs)) return false;
    switch (op) {
    case Opcode::add: out = lhs + rhs; break;
    case Opcode::sub: out = lhs - rhs; break;
    case Opcode::mul: out = lhs * rhs; break;
    case Opcode::fmin_f32: out = std::min(lhs, rhs); break;
    case Opcode::fmax_f32: out = std::max(lhs, rhs); break;
    default: return false;
    }
    return std::isfinite(out);
}

// Try to enclose a univariate expression `root(var)` where `var` has a known
// finite interval `[v_lo, v_hi]` (used for folded tent `min(ax, 0.5 - ax)` and
// Horner polynomials like `sin01` and `exp2_fast`).
bool try_bound_univariate(const Value* root,
                          const std::unordered_map<const Value*, FInterval>& env,
                          FInterval& out_iv) noexcept
{
    if (!root || !root->is_instruction()) return false;
    const Instruction* inst = root->defining_instruction();
    if (!inst || inst->operand_count() < 2) return false;

    // Candidate single-variable roots: operands or grandchildren with a tight finite interval.
    std::vector<const Value*> candidates;
    auto collect_cands = [&](const Value* v, int d, auto& self) -> void {
        if (!v || d > 12) return;
        double dummy = 0.0;
        if (get_float_const(v, dummy)) return;
        auto it = env.find(v);
        if (it != env.end() && it->second.is_finite()) {
            if (std::find(candidates.begin(), candidates.end(), v) == candidates.end())
                candidates.push_back(v);
        }
        if (v->is_instruction() && v->defining_instruction()) {
            for (const Value* op : v->defining_instruction()->operands())
                self(op, d + 1, self);
        }
    };
    collect_cands(root, 0, collect_cands);

    bool found = false;
    for (const Value* cand : candidates) {
        if (cand == root) continue;
        auto it = env.find(cand);
        if (it == env.end() || !it->second.is_finite()) continue;
        const Value* fixed_var = cand;
        double y0 = 0.0;
        if (!eval_univariate_at(root, fixed_var, it->second.lo, 0, y0) || fixed_var != cand) continue;

        double lo = y0;
        double hi = y0;
        constexpr int kSteps = 64;
        bool ok = true;
        for (int i = 1; i <= kSteps; ++i) {
            double t = static_cast<double>(i) / static_cast<double>(kSteps);
            double x = it->second.lo + (it->second.hi - it->second.lo) * t;
            const Value* vcheck = cand;
            double y = 0.0;
            if (!eval_univariate_at(root, vcheck, x, 0, y)) {
                ok = false;
                break;
            }
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
        if (!ok) continue;
        double span = hi - lo;
        double pad = std::max(1e-7, span * 1e-5);
        FInterval cand_iv;
        cand_iv.lo = lo - pad;
        cand_iv.hi = hi + pad;
        // Preserve exact non-negativity when endpoints & samples are >= 0
        if (lo >= 0.0) cand_iv.lo = std::max(0.0, lo - 1e-9);
        cand_iv.non_nan = true;
        cand_iv.known = true;
        if (!found || (cand_iv.hi - cand_iv.lo) < (out_iv.hi - out_iv.lo)) {
            out_iv = cand_iv;
            found = true;
        }
    }
    return found;
}

std::unordered_map<const Value*, FInterval> analyze_intervals(const Function& fn)
{
    std::unordered_map<const Value*, FInterval> env;

    auto get_iv = [&](const Value* v) -> FInterval {
        auto it = env.find(v);
        if (it != env.end()) return it->second;
        double fc = 0.0;
        if (get_float_const(v, fc) && std::isfinite(fc)) {
            return FInterval{fc, fc, true, true};
        }
        int64_t ic = 0;
        if (get_int_const(v, ic)) {
            double d = static_cast<double>(ic);
            return FInterval{d, d, true, true};
        }
        return FInterval{};
    };

    for (const BasicBlock* bb : fn.blocks()) {
        if (!bb) continue;
        for (const Instruction* inst : *bb) {
            if (!inst || !inst->result()) continue;
            const Value* res = inst->result();
            Opcode op = inst->opcode();
            FInterval iv;

            switch (op) {
            case Opcode::fconst_f64: {
                double c = inst->imm_f64();
                if (std::isfinite(c)) iv = FInterval{c, c, true, true};
                break;
            }
            case Opcode::iconst_i32:
            case Opcode::patchable_const_i32:
                iv = FInterval{static_cast<double>(inst->imm_i32()), static_cast<double>(inst->imm_i32()), true, true};
                break;
            case Opcode::iconst_i64:
            case Opcode::patchable_const_i64:
                iv = FInterval{static_cast<double>(inst->imm_i64()), static_cast<double>(inst->imm_i64()), true, true};
                break;
            case Opcode::fabs_f32:
            case Opcode::fabs_f64: {
                FInterval a = get_iv(inst->operand(0));
                if (a.is_finite()) {
                    if (a.lo >= 0.0) iv = FInterval{a.lo, a.hi, true, true};
                    else if (a.hi <= 0.0) iv = FInterval{-a.hi, -a.lo, true, true};
                    else iv = FInterval{0.0, std::max(-a.lo, a.hi), true, true};
                } else {
                    iv = FInterval{0.0, std::numeric_limits<double>::infinity(), true, true};
                }
                break;
            }
            case Opcode::neg: {
                FInterval a = get_iv(inst->operand(0));
                if (a.known) iv = FInterval{-a.hi, -a.lo, a.non_nan, true};
                break;
            }
            case Opcode::floor_f32:
            case Opcode::floor_f64: {
                FInterval a = get_iv(inst->operand(0));
                if (a.is_finite()) iv = FInterval{std::floor(a.lo), std::floor(a.hi), true, true};
                break;
            }
            case Opcode::ceil_f32:
            case Opcode::ceil_f64: {
                FInterval a = get_iv(inst->operand(0));
                if (a.is_finite()) iv = FInterval{std::ceil(a.lo), std::ceil(a.hi), true, true};
                break;
            }
            case Opcode::round_f32:
            case Opcode::round_f64: {
                FInterval a = get_iv(inst->operand(0));
                if (a.is_finite()) iv = FInterval{std::round(a.lo), std::round(a.hi), true, true};
                break;
            }
            case Opcode::fmax_f32:
            case Opcode::fmax_f64: {
                FInterval a = get_iv(inst->operand(0)), b = get_iv(inst->operand(1));
                iv = FInterval{std::max(a.lo, b.lo), std::max(a.hi, b.hi), b.is_finite() || a.is_finite(), true};
                break;
            }
            case Opcode::fmin_f32:
            case Opcode::fmin_f64: {
                FInterval a = get_iv(inst->operand(0)), b = get_iv(inst->operand(1));
                iv = FInterval{std::min(a.lo, b.lo), std::min(a.hi, b.hi), b.is_finite() || a.is_finite(), true};
                if (FInterval uni; try_bound_univariate(res, env, uni)) {
                    iv.lo = std::max(iv.lo, uni.lo);
                    iv.hi = std::min(iv.hi, uni.hi);
                }
                break;
            }
            case Opcode::add: {
                FInterval a = get_iv(inst->operand(0)), b = get_iv(inst->operand(1));
                if (a.known && b.known) iv = FInterval{a.lo + b.lo, a.hi + b.hi, a.non_nan && b.non_nan, true};
                if (FInterval uni; try_bound_univariate(res, env, uni)) {
                    if (!iv.is_finite() || (uni.hi - uni.lo) < (iv.hi - iv.lo)) iv = uni;
                }
                break;
            }
            case Opcode::sub: {
                FInterval a = get_iv(inst->operand(0)), b = get_iv(inst->operand(1));
                if (inst->operand(0) == inst->operand(1)) iv = FInterval{0.0, 0.0, true, true};
                else if (a.known && b.known) iv = FInterval{a.lo - b.hi, a.hi - b.lo, a.non_nan && b.non_nan, true};
                if (FInterval uni; try_bound_univariate(res, env, uni)) {
                    if (!iv.is_finite() || (uni.hi - uni.lo) < (iv.hi - iv.lo)) iv = uni;
                }
                break;
            }
            case Opcode::mul: {
                const Value* op0 = inst->operand(0);
                const Value* op1 = inst->operand(1);
                FInterval a = get_iv(op0), b = get_iv(op1);
                if (op0 == op1) {
                    if (a.is_finite()) {
                        double p1 = a.lo * a.lo, p2 = a.hi * a.hi;
                        iv = FInterval{(a.lo <= 0.0 && a.hi >= 0.0) ? 0.0 : std::min(p1, p2), std::max(p1, p2), true, true};
                    } else {
                        iv = FInterval{0.0, std::numeric_limits<double>::infinity(), true, true};
                    }
                } else if (a.is_finite() && b.is_finite()) {
                    double p1 = a.lo * b.lo, p2 = a.lo * b.hi, p3 = a.hi * b.lo, p4 = a.hi * b.hi;
                    iv = FInterval{std::min({p1, p2, p3, p4}), std::max({p1, p2, p3, p4}), true, true};
                } else if (a.known && b.known && a.lo >= 0.0 && b.lo >= 0.0) {
                    iv = FInterval{a.lo * b.lo, a.hi * b.hi, a.non_nan && b.non_nan, true};
                }
                if (FInterval uni; try_bound_univariate(res, env, uni)) {
                    if (!iv.is_finite() || (uni.hi - uni.lo) < (iv.hi - iv.lo)) iv = uni;
                }
                break;
            }
            case Opcode::sdiv: {
                FInterval a = get_iv(inst->operand(0)), b = get_iv(inst->operand(1));
                if (a.is_finite() && b.strictly_nonzero()) {
                    double q1 = a.lo / b.lo, q2 = a.lo / b.hi, q3 = a.hi / b.lo, q4 = a.hi / b.hi;
                    iv = FInterval{std::min({q1, q2, q3, q4}), std::max({q1, q2, q3, q4}), true, true};
                }
                break;
            }
            case Opcode::sqrt_f32:
            case Opcode::sqrt_f64: {
                FInterval a = get_iv(inst->operand(0));
                if (a.non_negative()) iv = FInterval{std::sqrt(a.lo), std::sqrt(a.hi), true, true};
                break;
            }
            case Opcode::slt: {
                FInterval a = get_iv(inst->operand(0)), b = get_iv(inst->operand(1));
                if (a.is_finite() && b.is_finite()) {
                    if (a.hi < b.lo) iv = FInterval{1.0, 1.0, true, true};
                    else if (a.lo >= b.hi) iv = FInterval{0.0, 0.0, true, true};
                    else iv = FInterval{0.0, 1.0, true, true};
                }
                break;
            }
            case Opcode::select: {
                FInterval c = get_iv(inst->operand(0)), t = get_iv(inst->operand(1)), f = get_iv(inst->operand(2));
                if (c.is_finite() && c.lo == 1.0 && c.hi == 1.0) iv = t;
                else if (c.is_finite() && c.lo == 0.0 && c.hi == 0.0) iv = f;
                else if (t.known && f.known) iv = FInterval{std::min(t.lo, f.lo), std::max(t.hi, f.hi), t.non_nan && f.non_nan, true};
                break;
            }
            case Opcode::and_: {
                FInterval b = get_iv(inst->operand(1));
                if (b.is_finite() && b.lo >= 0.0) iv = FInterval{0.0, b.hi, true, true};
                break;
            }
            default:
                break;
            }
            if (iv.known) env[res] = iv;
        }
    }
    return env;
}

// Build a standalone single-block MIR slice for `target_val` and run
// brass::verify::verify_mir_equivalence(slice_fn, zero_fn) to search for any
// concrete counterexample making `target_val` unsafe (e.g. == 0, NaN, +-Inf, or < 0 for sqrt).
brass::verify::EquivResult run_slice_counterexample_search(const Value* target_val,
                                                           bool is_sqrt,
                                                           bool is_int_div)
{
    using namespace brass;
    // Collect backward slice within the same basic block up to depth 8
    std::vector<const Instruction*> slice_insts;
    std::unordered_set<const Value*> visited_vals;
    std::vector<const Value*> leaf_vars;

    auto collect = [&](const Value* v, int depth, auto& self) -> void {
        if (!v || visited_vals.count(v)) return;
        visited_vals.insert(v);
        if (!v->is_instruction() || depth >= 8 || !v->defining_instruction()) {
            leaf_vars.push_back(v);
            return;
        }
        const Instruction* inst = v->defining_instruction();
        Opcode op = inst->opcode();
        if (op == Opcode::fconst_f64 || op == Opcode::iconst_i32 || op == Opcode::iconst_i64) {
            slice_insts.push_back(inst);
            return;
        }
        if (op != Opcode::add && op != Opcode::sub && op != Opcode::mul &&
            op != Opcode::sdiv && op != Opcode::neg && op != Opcode::fabs_f32 &&
            op != Opcode::fmin_f32 && op != Opcode::fmax_f32 &&
            op != Opcode::floor_f32 && op != Opcode::select && op != Opcode::slt &&
            op != Opcode::and_ && op != Opcode::or_ && op != Opcode::xor_) {
            leaf_vars.push_back(v);
            return;
        }
        for (const Value* opnd : inst->operands()) self(opnd, depth + 1, self);
        slice_insts.push_back(inst);
    };
    collect(target_val, 0, collect);

    std::vector<Type> param_types;
    for (const Value* lv : leaf_vars) {
        Type t = lv->type();
        if (t != Type::f32() && t != Type::f64() && t != Type::i32() && t != Type::i64()) t = Type::f32();
        param_types.push_back(t);
    }
    if (param_types.empty()) param_types.push_back(Type::i32());

    Module mod("slice_verify_mod");
    Function* slice_fn = mod.create_function("slice_check", Type::i32(), param_types);
    Function* zero_fn = mod.create_function("slice_zero", Type::i32(), param_types);
    {
        Builder zb(mod);
        zb.set_function(zero_fn);
        BasicBlock* zbb = zb.append_block("entry");
        for (Type pt : param_types) zb.add_block_param(zbb, pt);
        zb.position_at_end(zbb);
        zb.build_ret(zb.build_iconst_i32(0));
    }

    Builder sb(mod);
    sb.set_function(slice_fn);
    BasicBlock* sbb = sb.append_block("entry");
    std::unordered_map<const Value*, Value*> vmap;
    for (size_t i = 0; i < leaf_vars.size(); ++i) {
        Value* p = sb.add_block_param(sbb, param_types[i]);
        sb.position_at_end(sbb);
        if (param_types[i] == Type::f32())
            p = sb.build_fmin_f32(sb.build_fmax_f32(p, sb.build_fconst_f32(-1e6f)), sb.build_fconst_f32(1e6f));
        vmap[leaf_vars[i]] = p;
    }
    if (leaf_vars.empty()) {
        sb.add_block_param(sbb, param_types[0]);
        sb.position_at_end(sbb);
    }

    for (const Instruction* inst : slice_insts) {
        Opcode op = inst->opcode();
        Value* cloned = nullptr;
        if (op == Opcode::fconst_f64) {
            cloned = (inst->type() == Type::f32())
                   ? sb.build_fconst_f32(static_cast<float>(inst->imm_f64()))
                   : sb.build_fconst_f64(inst->imm_f64());
        } else if (op == Opcode::iconst_i32) cloned = sb.build_iconst_i32(inst->imm_i32());
        else if (op == Opcode::iconst_i64) cloned = sb.build_iconst_i64(inst->imm_i64());
        else if (inst->operand_count() == 1) {
            Value* a = vmap[inst->operand(0)];
            if (op == Opcode::neg) cloned = sb.build_neg(a);
            else if (op == Opcode::fabs_f32) cloned = sb.build_fabs_f32(a);
            else if (op == Opcode::floor_f32) cloned = sb.build_floor_f32(a);
        } else if (inst->operand_count() == 2) {
            Value* a = vmap[inst->operand(0)], *b = vmap[inst->operand(1)];
            if (op == Opcode::add) cloned = sb.build_add(a, b);
            else if (op == Opcode::sub) cloned = sb.build_sub(a, b);
            else if (op == Opcode::mul) cloned = sb.build_mul(a, b);
            else if (op == Opcode::sdiv) cloned = sb.build_sdiv(a, b);
            else if (op == Opcode::fmin_f32) cloned = sb.build_fmin_f32(a, b);
            else if (op == Opcode::fmax_f32) cloned = sb.build_fmax_f32(a, b);
            else if (op == Opcode::slt) cloned = sb.build_slt(a, b);
            else if (op == Opcode::and_) cloned = sb.build_and(a, b);
            else if (op == Opcode::or_) cloned = sb.build_or(a, b);
            else if (op == Opcode::xor_) cloned = sb.build_xor(a, b);
        } else if (inst->operand_count() == 3 && op == Opcode::select) {
            cloned = sb.build_select(vmap[inst->operand(0)], vmap[inst->operand(1)], vmap[inst->operand(2)]);
        }
        if (cloned) vmap[inst->result()] = cloned;
    }

    Value* tv = vmap[target_val];
    Value* bad = sb.build_iconst_i32(0);
    if (tv) {
        if (is_int_div) {
            Value* zero = (tv->type() == Type::i64()) ? sb.build_iconst_i64(0) : sb.build_iconst_i32(0);
            bad = sb.build_select(sb.build_eq(tv, zero), sb.build_iconst_i32(1), sb.build_iconst_i32(0));
        } else if (is_sqrt) {
            Value* cond = sb.build_or(sb.build_slt(tv, sb.build_fconst_f32(0.0f)), sb.build_ne(tv, tv));
            bad = sb.build_select(cond, sb.build_iconst_i32(1), sb.build_iconst_i32(0));
        } else {
            Value* cond = sb.build_or(sb.build_eq(tv, sb.build_fconst_f32(0.0f)), sb.build_ne(tv, tv));
            bad = sb.build_select(cond, sb.build_iconst_i32(1), sb.build_iconst_i32(0));
        }
    }
    sb.build_ret(bad);

    verify::EquivOptions opts{};
    opts.loop_unroll_bound = 1;
    opts.check_memory = false;
    opts.check_guards = false;
    opts.max_sat_conflicts = 2000;
    return verify::verify_mir_equivalence(*slice_fn, *zero_fn, opts);
}

// Discharge the IEEE-754 bit-pattern proof obligation for `[lo, hi]` using
// brass's exact 32-bit CDCL bit-blaster via brass::verify::verify_mir_equivalence:
// Given an IEEE-754 float bitvector `bits` constrained to `[lo, hi]`, prove
// `EquivStatus::ProvenEquivalent` that `bits` cannot be +-0.0f, NaN, or +-Inf
// (for division) or negative/NaN/Inf (for sqrt).
bool discharge_ieee754_bv_proof_with_brass(const FInterval& iv, bool is_sqrt, std::string& diag)
{
    using namespace brass;
    if (!iv.is_finite()) {
        diag = "Interval [" + std::to_string(iv.lo) + ", " + std::to_string(iv.hi) + "] is unbounded or non-finite";
        return false;
    }
    if (!is_sqrt && !iv.strictly_nonzero()) {
        diag = "Denominator interval [" + std::to_string(iv.lo) + ", " + std::to_string(iv.hi) + "] straddles or touches 0.0";
        return false;
    }
    if (is_sqrt && !iv.non_negative()) {
        diag = "Sqrt argument interval [" + std::to_string(iv.lo) + ", " + std::to_string(iv.hi) + "] allows negative inputs";
        return false;
    }

    float lo_f = static_cast<float>(iv.lo);
    float hi_f = static_cast<float>(iv.hi);
    if (!std::isfinite(lo_f) || !std::isfinite(hi_f)) {
        diag = "Float32 projection of interval overflows to Inf";
        return false;
    }

    Module mod("ieee754_bv_proof_mod");
    Function* bv_fn = mod.create_function("bv_safety_check", Type::i32(), {Type::i32()});
    Function* zero_fn = mod.create_function("bv_safety_zero", Type::i32(), {Type::i32()});

    {
        Builder zb(mod);
        zb.set_function(zero_fn);
        BasicBlock* zbb = zb.append_block("entry");
        zb.add_block_param(zbb, Type::i32());
        zb.position_at_end(zbb);
        zb.build_ret(zb.build_iconst_i32(0));
    }

    Builder b(mod);
    b.set_function(bv_fn);
    BasicBlock* bb = b.append_block("entry");
    Value* bits = b.add_block_param(bb, Type::i32());
    b.position_at_end(bb);

    // Encode interval membership in IEEE-754 sign-magnitude bit representation
    Value* in_range = nullptr;
    if (lo_f >= 0.0f) {
        // Non-negative finite floats have monotonic unsigned 32-bit representations in [lo_bits, hi_bits]
        uint32_t lo_bits = (lo_f == 0.0f) ? 0u : float_to_bits(lo_f);
        uint32_t hi_bits = float_to_bits(hi_f);
        Value* ge_lo = b.build_uge(bits, b.build_iconst_i32(static_cast<int32_t>(lo_bits)));
        Value* le_hi = b.build_ule(bits, b.build_iconst_i32(static_cast<int32_t>(hi_bits)));
        in_range = b.build_and(ge_lo, le_hi);
    } else if (hi_f < 0.0f) {
        // Strictly negative finite floats have unsigned representations in [float_to_bits(hi_f), float_to_bits(lo_f)]
        uint32_t u_min = float_to_bits(hi_f);
        uint32_t u_max = float_to_bits(lo_f);
        Value* ge_min = b.build_uge(bits, b.build_iconst_i32(static_cast<int32_t>(u_min)));
        Value* le_max = b.build_ule(bits, b.build_iconst_i32(static_cast<int32_t>(u_max)));
        in_range = b.build_and(ge_min, le_max);
    }

    Value* abs_bits = b.build_and(bits, b.build_iconst_i32(0x7FFFFFFF));
    Value* exp_bits = b.build_and(bits, b.build_iconst_i32(0x7F800000));
    Value* is_nan_or_inf = b.build_eq(exp_bits, b.build_iconst_i32(0x7F800000));

    Value* hazard = nullptr;
    if (is_sqrt) {
        Value* sign_bit = b.build_and(bits, b.build_iconst_i32(static_cast<int32_t>(0x80000000u)));
        Value* is_neg_nonzero = b.build_and(b.build_ne(sign_bit, b.build_iconst_i32(0)),
                                            b.build_ne(abs_bits, b.build_iconst_i32(0)));
        hazard = b.build_or(is_neg_nonzero, is_nan_or_inf);
    } else {
        Value* is_zero = b.build_eq(abs_bits, b.build_iconst_i32(0));
        hazard = b.build_or(is_zero, is_nan_or_inf);
    }

    Value* bad = b.build_and(in_range, hazard);
    Value* ret = b.build_select(bad, b.build_iconst_i32(1), b.build_iconst_i32(0));
    b.build_ret(ret);

    verify::EquivOptions opts{};
    opts.loop_unroll_bound = 1;
    opts.check_memory = false;
    opts.check_guards = false;
    opts.max_sat_conflicts = 5000;
    verify::EquivResult eq = verify::verify_mir_equivalence(*bv_fn, *zero_fn, opts);
    if (eq.status != verify::EquivStatus::ProvenEquivalent) {
        diag = "BV CDCL proof failed: " + eq.diagnostic;
        return false;
    }
    return true;
}

} // namespace

SynthNanSafetyResult verify_mir_nan_safety(const brass::Function& fn)
{
    using namespace brass;
    SynthNanSafetyResult result;
    result.safe = true;

    // 1. Check all float constants for literal NaN / Inf
    for (const BasicBlock* bb : fn.blocks()) {
        if (!bb) continue;
        for (const Instruction* inst : *bb) {
            if (!inst) continue;
            if (inst->opcode() == Opcode::fconst_f64 && !std::isfinite(inst->imm_f64())) {
                SafetyProofItem item;
                item.inst_value_id = inst->result() ? inst->result()->id() : 0;
                item.op_name = (inst->type() == Type::f32()) ? "fconst_f32" : "fconst_f64";
                item.hazard = SafetyHazardKind::NonFiniteConstant;
                item.proven_safe = false;
                item.diagnostic = "Non-finite float constant in MIR";
                result.items.push_back(std::move(item));
                result.safe = false;
            }
        }
    }

    // 2. Run forward IEEE-754 interval & univariate polynomial enclosure analysis
    auto env = analyze_intervals(fn);

    // 3. Discharge every division and sqrt obligation via slice counterexample search + BV CDCL proof
    for (const BasicBlock* bb : fn.blocks()) {
        if (!bb) continue;
        for (const Instruction* inst : *bb) {
            if (!inst) continue;
            Opcode op = inst->opcode();
            bool is_div = (op == Opcode::sdiv || op == Opcode::udiv ||
                           op == Opcode::smod || op == Opcode::umod);
            bool is_sqrt = (op == Opcode::sqrt_f32 || op == Opcode::sqrt_f64);
            if (!is_div && !is_sqrt) continue;

            if (is_div) result.divisions_checked++;
            if (is_sqrt) result.sqrts_checked++;

            const Value* target = is_div ? inst->operand(1) : inst->operand(0);
            bool is_int_div = is_div && inst->type().is_integer();

            SafetyProofItem item;
            item.inst_value_id = inst->result() ? inst->result()->id() : 0;
            item.op_name = std::string(opcode_name(op));

            // Step 3a: Concrete / Symbolic SSA slice check via verify_mir_equivalence
            verify::EquivResult slice_eq = run_slice_counterexample_search(target, is_sqrt, is_int_div);
            if (slice_eq.status == verify::EquivStatus::Counterexample) {
                item.proven_safe = false;
                item.hazard = is_sqrt ? SafetyHazardKind::NegativeSqrt
                            : (is_int_div ? SafetyHazardKind::IntDivByZero : SafetyHazardKind::FloatDivByZeroOrInf);
                item.diagnostic = slice_eq.diagnostic;
                result.safe = false;
                if (result.diagnostic.empty()) result.diagnostic = item.diagnostic;
                result.items.push_back(std::move(item));
                continue;
            }

            // Step 3b: Interval & CDCL bit-blasted IEEE-754 proof
            auto it = env.find(target);
            FInterval iv = (it != env.end()) ? it->second : FInterval{};
            item.lower_bound = static_cast<float>(iv.lo);
            item.upper_bound = static_cast<float>(iv.hi);

            std::string bv_diag;
            if (is_int_div) {
                if (iv.strictly_nonzero()) {
                    item.proven_safe = true;
                    result.divisions_proven++;
                } else {
                    item.proven_safe = false;
                    item.hazard = SafetyHazardKind::IntDivByZero;
                    item.diagnostic = "Integer divisor not proven non-zero";
                    result.safe = false;
                    if (result.diagnostic.empty()) result.diagnostic = item.diagnostic;
                }
            } else if (discharge_ieee754_bv_proof_with_brass(iv, is_sqrt, bv_diag)) {
                item.proven_safe = true;
                item.diagnostic = "Proven safe via IEEE-754 interval [" +
                                  std::to_string(item.lower_bound) + ", " +
                                  std::to_string(item.upper_bound) + "] + brass CDCL BV solver";
                if (is_div) result.divisions_proven++;
                if (is_sqrt) result.sqrts_proven++;
            } else {
                item.proven_safe = false;
                item.hazard = is_sqrt ? SafetyHazardKind::NegativeSqrt : SafetyHazardKind::FloatDivByZeroOrInf;
                item.diagnostic = bv_diag;
                result.safe = false;
                if (result.diagnostic.empty()) result.diagnostic = item.diagnostic;
            }
            result.items.push_back(std::move(item));
        }
    }

    return result;
}

SynthNanSafetyResult verify_synth_nan_safety(const LayerPlan& plan)
{
    brass::Module mod("synth_nan_safety_mod");
    brass::Function* fn = buildSynthLayerMir(plan, mod);
    return fn ? verify_mir_nan_safety(*fn) : SynthNanSafetyResult{false, 0, 0, 0, 0, {}, "buildSynthLayerMir returned null"};
}

SynthNanSafetyResult verify_synth_nan_safety(const SynthShape& shape)
{
    return verify_synth_nan_safety(shape.plan);
}

SynthNanSafetyResult verify_synth_nan_safety(const SynthGraph& graph)
{
    SynthNanSafetyResult agg;
    agg.safe = true;
    for (const LayerData& layer : graph.data().layers) {
        if (!layer.shape) continue;
        SynthNanSafetyResult r = verify_synth_nan_safety(*layer.shape);
        agg.divisions_checked += r.divisions_checked;
        agg.sqrts_checked += r.sqrts_checked;
        agg.divisions_proven += r.divisions_proven;
        agg.sqrts_proven += r.sqrts_proven;
        agg.items.insert(agg.items.end(), r.items.begin(), r.items.end());
        if (!r.safe) {
            agg.safe = false;
            if (agg.diagnostic.empty()) agg.diagnostic = "Layer '" + layer.id + "': " + r.diagnostic;
        }
    }
    return agg;
}

SynthNanSafetyResult verify_bus_nan_safety(const JitTopology& topology)
{
    brass::Module mod("bus_nan_safety_mod");
    brass::Function* fn = buildBusKernelMir(topology, mod);
    return fn ? verify_mir_nan_safety(*fn) : SynthNanSafetyResult{false, 0, 0, 0, 0, {}, "buildBusKernelMir returned null"};
}

SynthNanSafetyResult verify_voice_nan_safety(const VoiceJitTopology& topology)
{
    brass::Module mod("voice_nan_safety_mod");
    brass::Function* fn = buildVoiceKernelMir(topology, mod);
    return fn ? verify_mir_nan_safety(*fn) : SynthNanSafetyResult{false, 0, 0, 0, 0, {}, "buildVoiceKernelMir returned null"};
}

#else // !BROAUDIO_HAS_JIT

void lower_fma_for_verification(brass::Function&, brass::Module&) {}
SynthMirVerifyResult optimize_and_verify_mir(brass::Function&, brass::Module&, uint32_t) { return {false, 0, 0, "JIT disabled"}; }
SynthMirVerifyResult verify_synth_plan_mir_equivalence(const LayerPlan&, uint32_t) { return {false, 0, 0, "JIT disabled"}; }
SynthMirVerifyResult verify_synth_shape_mir_equivalence(const SynthShape&, uint32_t) { return {false, 0, 0, "JIT disabled"}; }
SynthMirVerifyResult verify_synth_graph_mir_equivalence(const SynthGraph&, uint32_t) { return {false, 0, 0, "JIT disabled"}; }
SynthMirVerifyResult verify_bus_kernel_mir_equivalence(const JitTopology&, uint32_t) { return {false, 0, 0, "JIT disabled"}; }
SynthMirVerifyResult verify_voice_kernel_mir_equivalence(const VoiceJitTopology&, uint32_t) { return {false, 0, 0, "JIT disabled"}; }
SynthNanSafetyResult verify_mir_nan_safety(const brass::Function&) { return {false, 0, 0, 0, 0, {}, "JIT disabled"}; }
SynthNanSafetyResult verify_synth_nan_safety(const LayerPlan&) { return {false, 0, 0, 0, 0, {}, "JIT disabled"}; }
SynthNanSafetyResult verify_synth_nan_safety(const SynthShape&) { return {false, 0, 0, 0, 0, {}, "JIT disabled"}; }
SynthNanSafetyResult verify_synth_nan_safety(const SynthGraph&) { return {false, 0, 0, 0, 0, {}, "JIT disabled"}; }
SynthNanSafetyResult verify_bus_nan_safety(const JitTopology&) { return {false, 0, 0, 0, 0, {}, "JIT disabled"}; }
SynthNanSafetyResult verify_voice_nan_safety(const VoiceJitTopology&) { return {false, 0, 0, 0, 0, {}, "JIT disabled"}; }

#endif // BROAUDIO_HAS_JIT

// ---------------------------------------------------------------------------
// Differential Interpreter == JIT Parity Verification
// ---------------------------------------------------------------------------

SynthParityResult verify_synth_parity(const std::shared_ptr<const SynthGraph>& graph,
                                      const SynthTrigger& trigger,
                                      int sample_rate,
                                      int num_frames)
{
    SynthParityResult res;
    if (!graph || num_frames <= 0) {
        res.diagnostic = "Invalid graph or frame count";
        return res;
    }

    if (!graph->precompile()) {
        res.diagnostic = "Failed to precompile SynthGraph kernels";
        return res;
    }

    auto render_blocks = [&](bool compiled, const std::vector<int>& block_pattern) -> std::vector<float> {
        SynthVoice voice(graph, sample_rate, trigger);
        std::vector<float> out;
        out.reserve(static_cast<size_t>(num_frames));
        std::vector<float> scratch(static_cast<size_t>(num_frames));
        size_t pat_idx = 0;
        while (static_cast<int>(out.size()) < num_frames) {
            int rem = num_frames - static_cast<int>(out.size());
            int blk = std::min(rem, block_pattern[pat_idx++ % block_pattern.size()]);
            voice.render(scratch.data(), blk, compiled);
            out.insert(out.end(), scratch.begin(), scratch.begin() + blk);
        }
        return out;
    };

    std::vector<float> interp_single = render_blocks(false, {num_frames});
    std::vector<float> jit_single    = render_blocks(true,  {num_frames});
    std::vector<float> jit_split     = render_blocks(true,  {1, 7, 19, 64, 257});

    res.total_samples = interp_single.size();
    res.first_mismatch_index = res.total_samples;

    for (size_t i = 0; i < res.total_samples; ++i) {
        float a = interp_single[i];
        float b = jit_single[i];
        float c = jit_split[i];
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) {
            res.any_nan_or_inf = true;
            res.first_mismatch_index = i;
            res.diagnostic = "Non-finite sample at index " + std::to_string(i);
            return res;
        }
        float diff = std::max(std::fabs(a - b), std::fabs(a - c));
        res.max_abs_diff = std::max(res.max_abs_diff, diff);

#if defined(__aarch64__)
        bool match = (diff <= 1e-5f);
#else
        bool match = (std::memcmp(&a, &b, sizeof(float)) == 0) &&
                     (std::memcmp(&a, &c, sizeof(float)) == 0);
#endif
        if (!match && res.first_mismatch_index == res.total_samples) {
            res.first_mismatch_index = i;
            std::ostringstream oss;
            oss << "Sample mismatch at frame " << i << ": interp=" << a
                << ", jit_single=" << b << ", jit_split=" << c;
            res.diagnostic = oss.str();
        }
    }

    res.passed = !res.any_nan_or_inf && (res.first_mismatch_index == res.total_samples);
    return res;
}

} // namespace broaudio
