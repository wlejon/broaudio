#include "jit_bus_builder.h"
#include "broaudio/dsp/jit/jit_types.h"

#include <brass/codegen/audio_builder.hpp>
#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/printer.hpp>
#include <brass/mir/types.hpp>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace broaudio {

std::shared_ptr<brass::codegen::KernelFunction> buildBusKernel(const JitTopology& topology)
{
    using namespace brass;
    using namespace brass::codegen;

    Module mod("bus_jit_module");
    mod.set_allow_fp_reassociation(true);

    Function* fn = mod.create_function("bus_jit_kernel", Type::void_type(), {
        Type::ptr(), // buffer
        Type::i32(), // numFrames
        Type::ptr(), // params
        Type::ptr()  // state
    });

    AudioKernelBuilder b(mod, fn);
    BasicBlock* entry = b.builder().append_block("entry");
    Value* buf_ptr    = b.builder().add_block_param(entry, Type::ptr());
    Value* num_frames = b.builder().add_block_param(entry, Type::i32());
    Value* params_ptr = b.builder().add_block_param(entry, Type::ptr());
    Value* state_ptr  = b.builder().add_block_param(entry, Type::ptr());
    b.position_at_end(entry);

    // Pre-load filter coefficients from params
    int k = topology.filterCount;
    if (k > JitTopology::MAX_FILTERS) k = JitTopology::MAX_FILTERS;

    struct CoeffVals {
        Value* b0;
        Value* b1;
        Value* b2;
        Value* a1;
        Value* a2;
    };
    std::vector<CoeffVals> coeffs(static_cast<size_t>(k));
    for (int f = 0; f < k; f++) {
        int32_t f_off = static_cast<int32_t>(offsetof(BusJitParams, filters) + f * sizeof(BiquadCoeffsPod));
        coeffs[f].b0 = b.load_f32(params_ptr, f_off + static_cast<int32_t>(offsetof(BiquadCoeffsPod, b0)));
        coeffs[f].b1 = b.load_f32(params_ptr, f_off + static_cast<int32_t>(offsetof(BiquadCoeffsPod, b1)));
        coeffs[f].b2 = b.load_f32(params_ptr, f_off + static_cast<int32_t>(offsetof(BiquadCoeffsPod, b2)));
        coeffs[f].a1 = b.load_f32(params_ptr, f_off + static_cast<int32_t>(offsetof(BiquadCoeffsPod, a1)));
        coeffs[f].a2 = b.load_f32(params_ptr, f_off + static_cast<int32_t>(offsetof(BiquadCoeffsPod, a2)));
    }

    // Pre-load initial filter states from state
    std::vector<Value*> inits;
    inits.reserve(static_cast<size_t>(k * 4));
    for (int f = 0; f < k; f++) {
        int32_t s_off_l = static_cast<int32_t>(offsetof(BusJitState, filterStateL) + f * sizeof(BiquadStatePod));
        int32_t s_off_r = static_cast<int32_t>(offsetof(BusJitState, filterStateR) + f * sizeof(BiquadStatePod));
        Value* z1_l = b.load_f32(state_ptr, s_off_l + static_cast<int32_t>(offsetof(BiquadStatePod, z1)));
        Value* z2_l = b.load_f32(state_ptr, s_off_l + static_cast<int32_t>(offsetof(BiquadStatePod, z2)));
        Value* z1_r = b.load_f32(state_ptr, s_off_r + static_cast<int32_t>(offsetof(BiquadStatePod, z1)));
        Value* z2_r = b.load_f32(state_ptr, s_off_r + static_cast<int32_t>(offsetof(BiquadStatePod, z2)));
        inits.push_back(z1_l);
        inits.push_back(z2_l);
        inits.push_back(z1_r);
        inits.push_back(z2_r);
    }

    // Pre-load distortion parameters if active
    Value* drive = nullptr;
    Value* mix = nullptr;
    Value* out_gain = nullptr;
    if (topology.hasDistortion) {
        drive    = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, distortionDrive)));
        mix      = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, distortionMix)));
        out_gain = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, distortionOutputGain)));
    }

    // Pre-load gain & pan ramp parameters if fused
    Value* gain_start = nullptr;
    Value* gain_step  = nullptr;
    Value* pan_l_start = nullptr;
    Value* pan_l_step  = nullptr;
    Value* pan_r_start = nullptr;
    Value* pan_r_step  = nullptr;
    if (topology.fuseGainPan) {
        gain_start  = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, gainStart)));
        gain_step   = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, gainStep)));
        pan_l_start = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, panLStart)));
        pan_l_step  = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, panLStep)));
        pan_r_start = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, panRStart)));
        pan_r_step  = b.load_f32(params_ptr, static_cast<int32_t>(offsetof(BusJitParams, panRStep)));
    }

    Value* start = b.const_i32(0);
    Value* end   = num_frames;
    Value* step  = b.const_i32(1);

    auto loop_body = [&](Value* idx, const std::vector<Value*>& accs) -> std::vector<Value*> {
        // Frame index idx -> interleaved sample offsets: L = 2*idx, R = 2*idx + 1
        Value* idx_l = b.add(idx, idx);
        Value* idx_r = b.add(idx_l, b.const_i32(1));

        Value* x_l = b.load_f32_indexed(buf_ptr, idx_l, 4);
        Value* x_r = b.load_f32_indexed(buf_ptr, idx_r, 4);

        std::vector<Value*> next_accs;
        next_accs.reserve(static_cast<size_t>(k * 4));

        // 1. Direct Form II Transposed Biquad Filter Cascade
        for (int f = 0; f < k; f++) {
            BiquadCoeffs c;
            c.b0 = coeffs[f].b0;
            c.b1 = coeffs[f].b1;
            c.b2 = coeffs[f].b2;
            c.a1 = coeffs[f].a1;
            c.a2 = coeffs[f].a2;

            BiquadState s_l{accs[static_cast<size_t>(f * 4 + 0)], accs[static_cast<size_t>(f * 4 + 1)]};
            BiquadState s_r{accs[static_cast<size_t>(f * 4 + 2)], accs[static_cast<size_t>(f * 4 + 3)]};

            b.biquad_df2t_stereo(c, s_l, s_r, x_l, x_r, x_l, x_r);

            next_accs.push_back(s_l.z1);
            next_accs.push_back(s_l.z2);
            next_accs.push_back(s_r.z1);
            next_accs.push_back(s_r.z2);
        }

        // 2. Waveshaping / Distortion
        if (topology.hasDistortion) {
            Value* dry_l = x_l;
            Value* dry_r = x_r;
            Value* driven_l = b.mul(x_l, drive);
            Value* driven_r = b.mul(x_r, drive);

            Value* wet_l = nullptr;
            Value* wet_r = nullptr;

            switch (topology.distortionMode) {
                case DistortionMode::SoftClip:
                    wet_l = b.soft_clip(driven_l);
                    wet_r = b.soft_clip(driven_r);
                    break;
                case DistortionMode::HardClip: {
                    Value* one = b.const_f32(1.0f);
                    wet_l = b.hard_clip(driven_l, one);
                    wet_r = b.hard_clip(driven_r, one);
                    break;
                }
                case DistortionMode::Foldback: {
                    Value* one = b.const_f32(1.0f);
                    wet_l = b.foldback(driven_l, one);
                    wet_r = b.foldback(driven_r, one);
                    break;
                }
                default:
                    wet_l = driven_l;
                    wet_r = driven_r;
                    break;
            }

            wet_l = b.mul(wet_l, out_gain);
            wet_r = b.mul(wet_r, out_gain);

            // Wet/Dry mix: dry + (wet - dry) * mix
            Value* diff_l = b.sub(wet_l, dry_l);
            Value* diff_r = b.sub(wet_r, dry_r);
            x_l = b.fma(diff_l, mix, dry_l);
            x_r = b.fma(diff_r, mix, dry_r);
        }

        // 3. Fused Gain & Pan Ramping
        if (topology.fuseGainPan) {
            Value* idx_f = b.i32_to_f32(idx);
            Value* cur_gain  = b.fma(idx_f, gain_step, gain_start);
            Value* cur_pan_l = b.fma(idx_f, pan_l_step, pan_l_start);
            Value* cur_pan_r = b.fma(idx_f, pan_r_step, pan_r_start);

            x_l = b.mul(b.mul(x_l, cur_gain), cur_pan_l);
            x_r = b.mul(b.mul(x_r, cur_gain), cur_pan_r);
        }

        // Store back in-place
        b.store_f32_indexed(buf_ptr, idx_l, x_l, 4);
        b.store_f32_indexed(buf_ptr, idx_r, x_r, 4);

        return next_accs;
    };

    if (k > 0) {
        std::vector<Value*> final_accs = b.for_range_reduce_n(start, end, step, inits, loop_body);

        // Store final filter states back to BusJitState
        for (int f = 0; f < k; f++) {
            int32_t s_off_l = static_cast<int32_t>(offsetof(BusJitState, filterStateL) + f * sizeof(BiquadStatePod));
            int32_t s_off_r = static_cast<int32_t>(offsetof(BusJitState, filterStateR) + f * sizeof(BiquadStatePod));
            b.store_f32(state_ptr, final_accs[static_cast<size_t>(f * 4 + 0)], s_off_l + static_cast<int32_t>(offsetof(BiquadStatePod, z1)));
            b.store_f32(state_ptr, final_accs[static_cast<size_t>(f * 4 + 1)], s_off_l + static_cast<int32_t>(offsetof(BiquadStatePod, z2)));
            b.store_f32(state_ptr, final_accs[static_cast<size_t>(f * 4 + 2)], s_off_r + static_cast<int32_t>(offsetof(BiquadStatePod, z1)));
            b.store_f32(state_ptr, final_accs[static_cast<size_t>(f * 4 + 3)], s_off_r + static_cast<int32_t>(offsetof(BiquadStatePod, z2)));
        }
    } else {
        // No filters: pure loop over frames
        b.for_range(start, end, step, [&](Value* idx) {
            loop_body(idx, {});
        });
    }

    b.builder().build_ret_void();

    KernelOptions opts = KernelOptions::audio_realtime();
    opts.enable_unroll = false;
    KernelJit jit(opts);
    KernelFunction kfn = jit.compile(mod, "bus_jit_kernel");

    if (!kfn.is_valid()) {
        throw std::runtime_error("buildBusKernel: JIT compilation failed to emit valid entry point");
    }

    return std::make_shared<KernelFunction>(std::move(kfn));
}

} // namespace broaudio
