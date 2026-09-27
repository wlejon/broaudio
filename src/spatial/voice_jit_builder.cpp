// Emits the compiled voice-chain kernels (spatial/voice_jit.h):
//
//   chain kernel:     void kernel(const VoiceJitArgs* a, VoiceChainState* s, int32_t n)
//   air lane kernel:  void kernel(float* x, AirLaneSection* sections, int32_t n)
//
// Bit-exactness with spatial_chain.cpp is the contract: every expression below
// is the interpreter's, operand for operand and in the same association, and
// the kernels are compiled without FMA contraction or FP reassociation. Change
// one side and the parity test (test_voice_jit) will say so.

#include "voice_jit_builder.h"

#if BROAUDIO_HAS_JIT
#include <brass/codegen/audio_builder.hpp>
#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/types.hpp>
#endif

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace broaudio {

#if BROAUDIO_HAS_JIT

namespace {

using brass::BasicBlock;
using brass::Builder;
using brass::Function;
using brass::Module;
using brass::Type;
using brass::Value;

constexpr int32_t off(size_t o) { return static_cast<int32_t>(o); }

#define ARG_OFF(field) off(offsetof(VoiceJitArgs, field))
#define ST_OFF(field) off(offsetof(VoiceChainState, field))
#define SEC_OFF(field) off(offsetof(AirLaneSection, field))

class Emitter {
public:
    explicit Emitter(Builder& b) : b_(b) {}

    void emitChain(const VoiceJitTopology& t, Value* A, Value* S, Value* N);
    void emitAir(Value* X, Value* SEC, Value* N);

private:
    Value* f(float v) { return b_.build_fconst_f32(v); }
    Value* i(int32_t v) { return b_.build_iconst_i32(v); }
    Value* ldf(Value* p, int32_t o) { return b_.build_load(Type::f32(), p, o); }
    Value* ldi(Value* p, int32_t o) { return b_.build_load(Type::i32(), p, o); }
    Value* ldp(Value* p, int32_t o) { return b_.build_load(Type::ptr(), p, o); }
    void stf(Value* p, int32_t o, Value* v) { b_.build_store(Type::f32(), p, o, v); }
    void sti(Value* p, int32_t o, Value* v) { b_.build_store(Type::i32(), p, o, v); }
    Value* add(Value* x, Value* y) { return b_.build_add(x, y); }
    Value* sub(Value* x, Value* y) { return b_.build_sub(x, y); }
    Value* mul(Value* x, Value* y) { return b_.build_mul(x, y); }
    // std::clamp(x, lo, hi) for ordered values.
    Value* clamp(Value* x, Value* lo, Value* hi) {
        return b_.build_fmin_f32(b_.build_fmax_f32(x, lo), hi);
    }
    Value* at(Value* base, Value* idx) { return b_.build_load_indexed(Type::f32(), base, idx, 4); }
    void put(Value* base, Value* idx, Value* v) { b_.build_store_indexed(Type::f32(), base, idx, 4, v); }

    Builder& b_;
};

// Every stage after air, one loop over the block's frames. The only values
// carried across frames are the delay line's cursor and smoother and the head
// filter's two one-poles; everything per-sample the glue already computed
// (gain, send and pan trajectories) is read as p[i * stride].
void Emitter::emitChain(const VoiceJitTopology& t, Value* A, Value* S, Value* N)
{
    const int nch = t.stereo ? 2 : 1;
    const bool delay = t.delay != VoiceJitTopology::Delay::None;
    const bool moving = t.delay == VoiceJitTopology::Delay::Interpolated;

    // --- Per-block parameters ---
    Value* ch[2] = {ldp(A, ARG_OFF(ch0)), nch == 2 ? ldp(A, ARG_OFF(ch1)) : nullptr};
    Value* bus = ldp(A, ARG_OFF(bus));
    Value* gain = ldp(A, ARG_OFF(gain));
    Value* gainStride = ldi(A, ARG_OFF(gainStride));
    Value* panL = ldp(A, ARG_OFF(panL));
    Value* panR = ldp(A, ARG_OFF(panR));
    Value* panStride = ldi(A, ARG_OFF(panStride));
    Value* line[2] = {};
    Value* mask = nullptr;
    Value* dTarget = nullptr;
    Value* dLo = nullptr;
    Value* dHi = nullptr;
    Value* dSmooth = nullptr;
    if (delay) {
        line[0] = ldp(A, ARG_OFF(line0));
        if (nch == 2) line[1] = ldp(A, ARG_OFF(line1));
        mask = ldi(A, ARG_OFF(delayMask));
        if (moving) {
            dTarget = ldf(A, ARG_OFF(delayTarget));
            dLo = ldf(A, ARG_OFF(delayLo));
            dHi = ldf(A, ARG_OFF(delayHi));
            dSmooth = ldf(A, ARG_OFF(delaySmooth));
        }
    }
    Value* hgL = nullptr;
    Value* hgR = nullptr;
    Value* hcL = nullptr;
    Value* hcR = nullptr;
    Value* hkL = nullptr;
    Value* hkR = nullptr;
    if (t.head) {
        hgL = ldf(A, ARG_OFF(headGainL));
        hgR = ldf(A, ARG_OFF(headGainR));
        hcL = ldf(A, ARG_OFF(headCoeffL));
        hcR = ldf(A, ARG_OFF(headCoeffR));
        hkL = sub(f(1.0f), hcL);
        hkR = sub(f(1.0f), hcR);
    }
    Value* send = nullptr;
    Value* sendAmt = nullptr;
    Value* sendStride = nullptr;
    if (t.send) {
        send = ldp(A, ARG_OFF(send));
        sendAmt = ldp(A, ARG_OFF(sendAmt));
        sendStride = ldi(A, ARG_OFF(sendStride));
    }

    // --- Carried state, loaded from its home in the VoiceChainState ---
    struct Home { int32_t off; bool isInt; };
    std::vector<Home> homes;
    auto slot = [&](int32_t o, bool isInt) {
        homes.push_back({o, isInt});
        return static_cast<int>(homes.size()) - 1;
    };
    int wSlot = -1, dSlot = -1, s1Slot = -1, s2Slot = -1, zlSlot = -1, zrSlot = -1;
    if (delay) {
        wSlot = slot(ST_OFF(writePos), true);
        dSlot = slot(ST_OFF(delayOut), false);
        if (moving) {
            s1Slot = slot(ST_OFF(delayS1), false);
            s2Slot = slot(ST_OFF(delayS2), false);
        }
    }
    if (t.head) {
        zlSlot = slot(ST_OFF(head) + off(offsetof(SpatialFilter, zL)), false);
        zrSlot = slot(ST_OFF(head) + off(offsetof(SpatialFilter, zR)), false);
    }
    std::vector<Value*> init;
    for (const Home& h : homes) init.push_back(h.isInt ? ldi(S, h.off) : ldf(S, h.off));

    auto body = [&](Value* idx, const std::vector<Value*>& in) -> std::vector<Value*> {
        std::vector<Value*> acc = in;
        Value* x[2] = {at(ch[0], idx), nch == 2 ? at(ch[1], idx) : nullptr};

        // Delay (chainDelay): write, then a 4-point Hermite read `d` behind.
        if (delay) {
            Value* d = acc[dSlot];
            if (moving) {
                Value* s1 = add(acc[s1Slot], mul(dSmooth, sub(dTarget, acc[s1Slot])));
                Value* s2 = add(acc[s2Slot], mul(dSmooth, sub(s1, acc[s2Slot])));
                d = add(d, clamp(sub(s2, d), f(-1.0f), f(0.5f)));
                d = clamp(d, dLo, dHi);
                acc[s1Slot] = s1;
                acc[s2Slot] = s2;
                acc[dSlot] = d;
            }
            Value* w = acc[wSlot];
            Value* rp = sub(b_.build_sitofp_f32_i32(w), d);
            Value* fl = b_.build_floor_f32(rp);
            Value* fr = sub(rp, fl);
            Value* i0 = b_.build_fptosi_i32_f32(fl);
            Value* im1 = b_.build_and(sub(i0, i(1)), mask);
            Value* i00 = b_.build_and(i0, mask);
            Value* i1 = b_.build_and(add(i0, i(1)), mask);
            Value* i2 = b_.build_and(add(i0, i(2)), mask);
            for (int cc = 0; cc < nch; ++cc) {
                put(line[cc], w, x[cc]);
                Value* xm1 = at(line[cc], im1);
                Value* x0 = at(line[cc], i00);
                Value* x1 = at(line[cc], i1);
                Value* x2 = at(line[cc], i2);
                Value* c1 = mul(f(0.5f), sub(x1, xm1));
                Value* c2 = sub(add(sub(xm1, mul(f(2.5f), x0)), mul(f(2.0f), x1)), mul(f(0.5f), x2));
                Value* c3 = add(mul(f(0.5f), sub(x2, xm1)), mul(f(1.5f), sub(x0, x1)));
                x[cc] = add(mul(add(mul(add(mul(c3, fr), c2), fr), c1), fr), x0);
            }
            acc[wSlot] = b_.build_and(add(w, i(1)), mask);
        }

        // Gain/pan (chainGainPan).
        Value* g = at(gain, mul(idx, gainStride));
        Value* pi = mul(idx, panStride);
        Value* pl = at(panL, pi);
        Value* pr = at(panR, pi);
        Value* L;
        Value* R;
        if (nch == 2) {
            Value* sl = mul(x[0], g);
            Value* sr = mul(x[1], g);
            L = add(mul(sl, pl), mul(sr, sub(f(1.0f), pr)));
            R = add(mul(sr, pr), mul(sl, sub(f(1.0f), pl)));
        } else {
            Value* v = mul(x[0], g);
            L = mul(v, pl);
            R = mul(v, pr);
        }

        // Head shadow + occlusion (SpatialFilter::process): ILD gain, then a
        // one-pole per ear.
        if (t.head) {
            L = mul(L, hgL);
            R = mul(R, hgR);
            L = acc[zlSlot] = add(mul(acc[zlSlot], hcL), mul(L, hkL));
            R = acc[zrSlot] = add(mul(acc[zrSlot], hcR), mul(R, hkR));
        }

        // Taps (chainTaps).
        Value* iL = add(idx, idx);
        Value* iR = add(iL, i(1));
        put(bus, iL, add(at(bus, iL), L));
        put(bus, iR, add(at(bus, iR), R));
        if (t.send) {
            Value* a = at(sendAmt, mul(idx, sendStride));
            put(send, iL, add(at(send, iL), mul(L, a)));
            put(send, iR, add(at(send, iR), mul(R, a)));
        }
        return acc;
    };

    brass::codegen::AudioKernelBuilder kb(b_);
    std::vector<Value*> out = kb.for_range_reduce_n(i(0), N, i(1), init, body);
    for (size_t h = 0; h < homes.size(); ++h)
        homes[h].isInt ? sti(S, homes[h].off, out[h]) : stf(S, homes[h].off, out[h]);
}

// chainAir over kAirLanes independent channels, one per lane of an f32x8.
// Section k of a frame depends only on section k-1's output of the same frame
// and on its own state, so the sections run one after another over the whole
// block: each loop carries four vectors (pole, mix, lowpass out, previous in)
// and the block's samples pass through memory between sections.
void Emitter::emitAir(Value* X, Value* SEC, Value* N)
{
    const Type v8 = Type::f32x8();
    Value* half = b_.build_vbroadcast(v8, f(0.5f));
    Value* one = b_.build_vbroadcast(v8, f(1.0f));
    Value* laneBytes = b_.build_iconst_i64(4 * kAirLanes);
    brass::codegen::AudioKernelBuilder kb(b_);
    for (int k = 0; k < kAirSections; ++k) {
        const int32_t base = off(sizeof(AirLaneSection)) * k;
        Value* dp = b_.build_vload(v8, SEC, base + SEC_OFF(dp));
        Value* dm = b_.build_vload(v8, SEC, base + SEC_OFF(dm));
        std::vector<Value*> init = {
            X,
            b_.build_vload(v8, SEC, base + SEC_OFF(p)),
            b_.build_vload(v8, SEC, base + SEC_OFF(m)),
            b_.build_vload(v8, SEC, base + SEC_OFF(z)),
            b_.build_vload(v8, SEC, base + SEC_OFF(xp)),
        };
        auto body = [&](Value*, const std::vector<Value*>& in) -> std::vector<Value*> {
            Value* ptr = in[0];
            Value* p = b_.build_vadd(in[1], dp);
            Value* m = b_.build_vadd(in[2], dm);
            Value* v = b_.build_vload(v8, ptr, 0);
            // z = 0.5 (1 - p) (v + xp) + p z;  xp = v;  v += m (z - v)
            Value* z = b_.build_vadd(b_.build_vmul(b_.build_vmul(half, b_.build_vsub(one, p)),
                                                   b_.build_vadd(v, in[4])),
                                     b_.build_vmul(p, in[3]));
            Value* out = b_.build_vadd(v, b_.build_vmul(m, b_.build_vsub(z, v)));
            b_.build_vstore(v8, ptr, 0, out);
            return {b_.build_add(ptr, laneBytes), p, m, z, v};
        };
        std::vector<Value*> out = kb.for_range_reduce_n(i(0), N, i(1), init, body);
        b_.build_vstore(v8, SEC, base + SEC_OFF(z), out[3]);
        b_.build_vstore(v8, SEC, base + SEC_OFF(xp), out[4]);
    }
}

#undef ARG_OFF
#undef ST_OFF
#undef SEC_OFF

template <typename Emit>
std::shared_ptr<brass::codegen::KernelFunction> compileKernel(Emit&& emit)
{
    using namespace brass::codegen;
    Module mod("voice_jit_module");
    mod.set_allow_fp_reassociation(false);
    Function* fn = mod.create_function("voice_jit_kernel", Type::void_type(),
                                       {Type::ptr(), Type::ptr(), Type::i32()});
    AudioKernelBuilder kb(mod, fn);
    Builder& b = kb.builder();
    BasicBlock* entry = b.append_block("entry");
    Value* a0 = b.add_block_param(entry, Type::ptr());
    Value* a1 = b.add_block_param(entry, Type::ptr());
    Value* n = b.add_block_param(entry, Type::i32());
    kb.position_at_end(entry);
    Emitter e(b);
    emit(e, a0, a1, n);
    b.build_ret_void();

    KernelOptions opts = KernelOptions::audio_realtime();
    // Bit-exact with the interpreter: no contraction, no reassociation.
    opts.enable_fma = false;
    opts.enable_fp_reassociation = false;
    opts.enable_unroll = false;
    KernelJit jit(opts);
    KernelFunction kfn = jit.compile(mod, "voice_jit_kernel");
    if (!kfn.is_valid())
        throw std::runtime_error("voice_jit: no entry point");
    return std::make_shared<KernelFunction>(std::move(kfn));
}

} // namespace

std::shared_ptr<brass::codegen::KernelFunction> buildVoiceKernel(const VoiceJitTopology& topology)
{
    if (topology.source || topology.air)
        throw std::invalid_argument("buildVoiceKernel: no source or air stage in a chain kernel");
    return compileKernel([&](Emitter& e, Value* a, Value* s, Value* n) { e.emitChain(topology, a, s, n); });
}

std::shared_ptr<brass::codegen::KernelFunction> buildAirLaneKernel()
{
    return compileKernel([](Emitter& e, Value* x, Value* sec, Value* n) { e.emitAir(x, sec, n); });
}

#else // !BROAUDIO_HAS_JIT

std::shared_ptr<brass::codegen::KernelFunction> buildVoiceKernel(const VoiceJitTopology&)
{
    return nullptr;
}

std::shared_ptr<brass::codegen::KernelFunction> buildAirLaneKernel()
{
    return nullptr;
}

#endif

} // namespace broaudio
