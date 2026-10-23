#pragma once

// Pure-MIR branchless math intrinsics and SMT speaker-protection safety prover
// for Verified JS Audio DSP Kernels.

#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/instruction.hpp>
#include <brass/mir/loop_opt.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/types.hpp>
#include <brass/verify/smt_equiv.hpp>

#include <cstdint>
#include <cstring>

namespace broaudio::dspmath {

using namespace brass;

constexpr float kS1 = 6.28318530717958648f;
constexpr float kS3 = -41.3417022403997548f;
constexpr float kS5 = 81.6052492760750372f;
constexpr float kS7 = -76.7058597530613288f;
constexpr float kS9 = 42.0586939448976611f;
constexpr float kS11 = -15.0946425768229819f;
constexpr float kInv2Pi = 0.159154943091895336f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kEulerE = 2.71828182845904523536f;
constexpr float kLn2 = 0.69314718055994530942f;
constexpr float kLn10 = 2.30258509299404568402f;
constexpr float kLog2E = 1.44269504088896340736f;
constexpr float kLog10E = 0.43429448190325182765f;
constexpr float kSqrt2 = 1.41421356237309504880f;
constexpr float kSqrt1_2 = 0.70710678118654752440f;

inline Value* emitSin01(Builder& b, Value* p) {
    Value* half = b.build_fconst_f32(0.5f);
    Value* x = b.build_sub(p, b.build_floor_f32(b.build_fadd(p, half)));
    Value* ax = b.build_fabs_f32(x);
    Value* a = b.build_fmin_f32(ax, b.build_sub(half, ax));
    Value* a2 = b.build_mul(a, a);
    Value* poly = b.build_fconst_f32(kS11);
    poly = b.build_fadd(b.build_fconst_f32(kS9), b.build_mul(a2, poly));
    poly = b.build_fadd(b.build_fconst_f32(kS7), b.build_mul(a2, poly));
    poly = b.build_fadd(b.build_fconst_f32(kS5), b.build_mul(a2, poly));
    poly = b.build_fadd(b.build_fconst_f32(kS3), b.build_mul(a2, poly));
    poly = b.build_fadd(b.build_fconst_f32(kS1), b.build_mul(a2, poly));
    Value* s = b.build_mul(a, poly);
    return b.build_select(b.build_slt(x, b.build_fconst_f32(0.0f)), b.build_neg(s), s);
}

inline Value* emitSinRad(Builder& b, Value* rad) {
    return emitSin01(b, b.build_mul(rad, b.build_fconst_f32(kInv2Pi)));
}

inline Value* emitCosRad(Builder& b, Value* rad) {
    return emitSin01(b, b.build_fadd(b.build_mul(rad, b.build_fconst_f32(kInv2Pi)),
                                     b.build_fconst_f32(0.25f)));
}

inline Value* emitTanh(Builder& b, Value* x) {
    Value* clamped = b.build_fmin_f32(b.build_fmax_f32(x, b.build_fconst_f32(-3.0f)),
                                      b.build_fconst_f32(3.0f));
    Value* x2 = b.build_mul(clamped, clamped);
    Value* c27 = b.build_fconst_f32(27.0f);
    Value* num = b.build_mul(clamped, b.build_fadd(x2, c27));
    Value* den = b.build_fadd(b.build_mul(x2, b.build_fconst_f32(9.0f)), c27);
    return b.build_sdiv(num, den);
}

inline Value* emitTrunc(Builder& b, Value* x) {
    return b.build_select(b.build_slt(x, b.build_fconst_f32(0.0f)),
                          b.build_ceil_f32(x), b.build_floor_f32(x));
}

inline Value* emitExp2(Builder& b, Value* x) {
    Value* xc = b.build_fmin_f32(b.build_fmax_f32(x, b.build_fconst_f32(-60.0f)),
                                 b.build_fconst_f32(60.0f));
    Value* ip_f = b.build_floor_f32(xc);
    Value* fp = b.build_sub(xc, ip_f);
    Value* p = b.build_fconst_f32(0.0135557f);
    p = b.build_fadd(b.build_fconst_f32(0.0520323f), b.build_mul(fp, p));
    p = b.build_fadd(b.build_fconst_f32(0.2413793f), b.build_mul(fp, p));
    p = b.build_fadd(b.build_fconst_f32(0.6930321f), b.build_mul(fp, p));
    p = b.build_fadd(b.build_fconst_f32(1.0f), b.build_mul(fp, p));
    Value* biased = b.build_add(b.build_fptosi_i32_f32(ip_f), b.build_iconst_i32(127));
    Value* scale = b.build_bitcast_f32_i32(b.build_shl(biased, b.build_iconst_i32(23)));
    return b.build_mul(scale, p);
}

inline Value* emitLog2(Builder& b, Value* x) {
    Value* xp = b.build_fmax_f32(x, b.build_fconst_f32(1e-30f));
    Value* bits = b.build_bitcast_i32_f32(xp);
    Value* exp_bits = b.build_and(b.build_lshr(bits, b.build_iconst_i32(23)), b.build_iconst_i32(255));
    Value* exp_f = b.build_sitofp_f32_i32(b.build_sub(exp_bits, b.build_iconst_i32(127)));
    Value* mant_bits = b.build_or(b.build_and(bits, b.build_iconst_i32(0x007FFFFF)),
                                  b.build_iconst_i32(0x3F800000));
    Value* t = b.build_sub(b.build_bitcast_f32_i32(mant_bits), b.build_fconst_f32(1.0f));
    Value* poly = b.build_fconst_f32(-0.0805908f);
    poly = b.build_fadd(b.build_fconst_f32(0.3211889f), b.build_mul(t, poly));
    poly = b.build_fadd(b.build_fconst_f32(-0.6777840f), b.build_mul(t, poly));
    poly = b.build_fadd(b.build_fconst_f32(1.4371860f), b.build_mul(t, poly));
    return b.build_fadd(exp_f, b.build_mul(t, poly));
}

inline Value* emitAtan(Builder& b, Value* x) {
    Value* ax = b.build_fabs_f32(x);
    Value* gt1 = b.build_sgt(ax, b.build_fconst_f32(1.0f));
    Value* z = b.build_select(gt1, b.build_sdiv(b.build_fconst_f32(1.0f), ax), ax);
    Value* z2 = b.build_mul(z, z);
    Value* num = b.build_mul(z, b.build_fadd(b.build_fconst_f32(1.0f),
                                             b.build_mul(b.build_fconst_f32(0.372003f), z2)));
    Value* den = b.build_fadd(b.build_fconst_f32(1.0f),
                              b.build_mul(z2, b.build_fadd(b.build_fconst_f32(0.703384f),
                                                           b.build_mul(b.build_fconst_f32(0.043562f), z2))));
    Value* r = b.build_sdiv(num, den);
    Value* folded = b.build_select(gt1, b.build_sub(b.build_fconst_f32(kHalfPi), r), r);
    return b.build_select(b.build_slt(x, b.build_fconst_f32(0.0f)), b.build_neg(folded), folded);
}

// Formally prove via Brass SMT / bitvector symbolic execution that the kernel's
// output is guaranteed finite (not NaN/Inf) and bounded within [-maxAmp, +maxAmp].
inline bool proveSpeakerProtectionSafety(const Function& kernelFn, float maxAmp) {
    Module checkMod("dsp_safety_proof");
    Function* checkFn = clone_function(kernelFn, checkMod);
    if (!checkFn || !checkFn->entry_block()) return false;

    BasicBlock* entry = checkFn->entry_block();
    Instruction* term = entry->tail();
    if (!term || term->opcode() != Opcode::ret || term->operand_count() == 0) return false;
    Value* retVal = term->operand(0);

    Builder b(checkMod);
    b.set_function(checkFn);
    b.position_before(term);

    // In IEEE-754 binary32, retVal is finite (neither NaN nor +-Inf) AND
    // satisfies -maxAmp <= retVal <= +maxAmp iff its 31-bit unsigned magnitude
    // (bitcast_i32_f32(retVal) & 0x7FFFFFFF) <=u bitcast_i32_f32(maxAmp).
    uint32_t maxBitsU32 = 0;
    std::memcpy(&maxBitsU32, &maxAmp, sizeof(float));
    Value* outBits = b.build_bitcast_i32_f32(retVal);
    Value* outMag = b.build_and(outBits, b.build_iconst_i32(0x7FFFFFFF));
    Value* badFlag = b.build_ugt(outMag, b.build_iconst_i32(static_cast<int32_t>(maxBitsU32)));
    Value* badF32 = b.build_select(badFlag, b.build_fconst_f32(1.0f), b.build_fconst_f32(0.0f));
    term->operands().clear();
    term->add_operand(badF32);

    Function* refFn = checkMod.create_function(
        "ref_zero", Type::f32(),
        {Type::f32(), Type::f32(), Type::f32(), Type::f32()});
    Builder rb(checkMod);
    rb.set_function(refFn);
    BasicBlock* refEntry = rb.append_block("entry");
    rb.add_block_param(refEntry, Type::f32());
    rb.add_block_param(refEntry, Type::f32());
    rb.add_block_param(refEntry, Type::f32());
    rb.add_block_param(refEntry, Type::f32());
    rb.position_at_end(refEntry);
    rb.build_ret(rb.build_fconst_f32(0.0f));

    brass::verify::EquivOptions eqOpts;
    eqOpts.check_memory = false;
    eqOpts.check_guards = false;
    auto eqRes = brass::verify::verify_mir_equivalence(*checkFn, *refFn, eqOpts);
    return eqRes.status == brass::verify::EquivStatus::ProvenEquivalent;
}

} // namespace broaudio::dspmath
