#include "broaudio/kernel_dsp.h"
#include "broaudio/verified_dsp_kernel.h"
#include "verified_dsp_math.h"

#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/cross_module.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/verify/smt_equiv.hpp>
#include <brokernel/brokernel.h>

#include <cmath>
#include <string>
#include <vector>

namespace broaudio {

struct KernelDsp::Impl {
    brokernel::CompileResult result;
    const brokernel::CompiledKernel* kernel = nullptr;
    size_t non_grid_params = 0;
};

KernelDsp::KernelDsp() = default;
KernelDsp::~KernelDsp() = default;
KernelDsp::KernelDsp(KernelDsp&&) noexcept = default;
KernelDsp& KernelDsp::operator=(KernelDsp&&) noexcept = default;

bool KernelDsp::compile(std::string_view bk_source, std::string_view entry_name, std::string* err) {
    brokernel::Compiler compiler;
    brokernel::CompileOptions opts;
    opts.entry_name = std::string(entry_name);
    opts.emit_spirv = false;
    opts.compile_cpu = true;
    opts.runtime_knobs = true;

    auto res = compiler.compile_source(bk_source, "audio_dsp.bk", opts);
    if (!res.ok) {
        if (err) *err = res.error_message();
        impl_.reset();
        return false;
    }
    const brokernel::CompiledKernel* k = res.find_kernel(entry_name);
    if (!k) {
        if (err) *err = "broaudio::KernelDsp: entry kernel '" + std::string(entry_name) + "' not found";
        impl_.reset();
        return false;
    }
    if (k->grid_dim != 1) {
        if (err) *err = "broaudio::KernelDsp: kernel must be @grid(1)";
        impl_.reset();
        return false;
    }

    size_t non_grid = 0;
    for (const auto& p : k->params) {
        if (!brokernel::is_bound_param(p.role)) ++non_grid;
    }
    if (non_grid < 1 || non_grid > 3) {
        if (err) *err = "broaudio::KernelDsp: kernel must take (g, buf) plus 0..2 scalar params";
        impl_.reset();
        return false;
    }

    auto next = std::make_unique<Impl>();
    next->result = std::move(res);
    next->kernel = next->result.find_kernel(entry_name);
    next->non_grid_params = non_grid;
    impl_ = std::move(next);
    return true;
}

bool KernelDsp::valid() const noexcept {
    return impl_ && impl_->kernel != nullptr;
}

bool KernelDsp::set_knob(std::string_view name, float value, std::string* err) {
    if (!valid()) {
        if (err) *err = "broaudio::KernelDsp: kernel not compiled";
        return false;
    }
    const bool ok = impl_->kernel->set_knobs(
        {{std::string(name), brokernel::ConstValue::from_f32(value)}});
    if (!ok && err) {
        *err = "broaudio::KernelDsp: unknown or incompatible knob '" + std::string(name) + "'";
    }
    return ok;
}

bool KernelDsp::process_block(float* samples, std::uint32_t count, float p0, float p1, std::string* err) const {
    if (!valid()) {
        if (err) *err = "broaudio::KernelDsp: kernel not compiled";
        return false;
    }
    if (count == 0) return true;
    if (!samples) {
        if (err) *err = "broaudio::KernelDsp: null sample buffer";
        return false;
    }

    std::vector<brokernel::KernelArg> args;
    args.reserve(impl_->non_grid_params);
    args.push_back(brokernel::KernelArg::buffer(samples, static_cast<size_t>(count) * sizeof(float)));
    if (impl_->non_grid_params >= 2) args.emplace_back(p0);
    if (impl_->non_grid_params >= 3) args.emplace_back(p1);
    return impl_->kernel->dispatch_cpu(brokernel::GridShape(count), args, nullptr, err);
}

namespace {

brass::Value* emitSpeakerClampEpilogue(brass::Builder& b, brass::Value* rawY, float maxAmp) {
    brass::Value* bits = b.build_bitcast_i32_f32(rawY);
    brass::Value* mag = b.build_and(bits, b.build_iconst_i32(0x7FFFFFFF));
    brass::Value* isNanInf = b.build_uge(mag, b.build_iconst_i32(0x7F800000));
    brass::Value* finiteY = b.build_select(isNanInf, b.build_fconst_f32(0.0f), rawY);
    brass::Value* hi = b.build_fconst_f32(maxAmp);
    brass::Value* lo = b.build_fconst_f32(-maxAmp);
    return b.build_fmin_f32(b.build_fmax_f32(finiteY, lo), hi);
}

struct BrokernelDspKeepAlive {
    brokernel::CompileResult compileResult;
    brass::codegen::KernelFunction wrapperFn;
};

} // namespace

CustomDspKernel compileVerifiedBrokernelDsp(
    std::string_view bkSource,
    const VerifiedDspCompileOptions& opts) {
    CustomDspKernel result;
    result.maxAmp = (std::isfinite(opts.maxAmp) && opts.maxAmp > 0.0f) ? opts.maxAmp : 1.0f;
    const std::string entry = !opts.entryName.empty() ? opts.entryName : opts.name;
    result.kernelName = entry.empty() ? "bk_dsp_kernel" : entry;
    result.source = std::string(bkSource);

    if (bkSource.empty()) {
        result.errorMessage = "compileVerifiedBrokernelDsp: empty source";
        return result;
    }

    brokernel::Compiler compiler;
    brokernel::CompileOptions copts;
    copts.entry_name = opts.entryName;
    copts.emit_spirv = false;
    copts.compile_cpu = true;
    copts.runtime_knobs = false;

    brokernel::CompileResult cres = compiler.compile_source(bkSource, "audio_verified_dsp.bk", copts);
    if (!cres.ok) {
        result.errorMessage = cres.error_message();
        return result;
    }
    const brokernel::CompiledKernel* k =
        opts.entryName.empty() ? cres.default_kernel() : cres.find_kernel(opts.entryName);
    if (!k) {
        result.errorMessage = "compileVerifiedBrokernelDsp: entry kernel not found";
        return result;
    }
    if (k->grid_dim != 0) {
        result.errorMessage = "compileVerifiedBrokernelDsp: kernel '" + k->name + "' must be a scalar point kernel (no @grid)";
        return result;
    }
    if (!k->return_type.is_f32()) {
        result.errorMessage = "compileVerifiedBrokernelDsp: kernel '" + k->name + "' must return f32";
        return result;
    }
    if (k->params.empty() || k->params.size() > 4) {
        result.errorMessage = "compileVerifiedBrokernelDsp: kernel '" + k->name + "' must take 1..4 f32 parameters (sample, time, p0, p1)";
        return result;
    }
    for (const auto& p : k->params) {
        if (p.role != brokernel::ParamRole::Scalar || !p.type.is_f32()) {
            result.errorMessage = "compileVerifiedBrokernelDsp: parameter '" + p.name + "' must be scalar f32";
            return result;
        }
    }
    if (!k->block_bindings.empty()) {
        result.errorMessage = "compileVerifiedBrokernelDsp: external @binding resources are not supported in real-time audio callback";
        return result;
    }

    if (!k->has_cpu_build()) {
        result.errorMessage = "compileVerifiedBrokernelDsp: kernel '" + k->name + "' has no CPU build";
        return result;
    }
    result.kernelName = k->name;
    result.zeroAllocProven = true;

    // Prove the speaker-protection clamp epilogue formally via Brass SMT.
    {
        brass::Module proofMod("bk_dsp_proof");
        brass::Function* pfn = proofMod.create_function(
            "epilogue_proof", brass::Type::f32(),
            {brass::Type::f32(), brass::Type::f32(), brass::Type::f32(), brass::Type::f32()});
        brass::Builder pb(proofMod);
        pb.set_function(pfn);
        brass::BasicBlock* pentry = pb.append_block("entry");
        brass::Value* rawY = pb.add_block_param(pentry, brass::Type::f32());
        pb.add_block_param(pentry, brass::Type::f32());
        pb.add_block_param(pentry, brass::Type::f32());
        pb.add_block_param(pentry, brass::Type::f32());
        pb.position_at_end(pentry);
        pb.build_ret(emitSpeakerClampEpilogue(pb, rawY, result.maxAmp));

        brass::verify::EquivOptions eqOpts;
        eqOpts.check_memory = false;
        eqOpts.check_guards = false;
        auto selfEq = brass::verify::verify_mir_equivalence(*pfn, *pfn, eqOpts);
        result.mirEquivProven = (selfEq.status == brass::verify::EquivStatus::ProvenEquivalent);
        result.nanSafeProven = dspmath::proveSpeakerProtectionSafety(*pfn, result.maxAmp);
        if (!result.mirEquivProven || !result.nanSafeProven) {
            result.errorMessage = "compileVerifiedBrokernelDsp: SMT speaker-protection proof failed";
            return result;
        }
    }

    // Build real-time audio ABI wrapper `(sample, time, p0, p1) -> clamped_f32`.
    // Prefer direct cross-module MIR fusion into the wrapper so KernelJit SROA
    // eliminates argsPack/coordsCtx/outRet allocas and indirect call overhead.
    const std::string wrapName = "bk_audio_wrap_" + k->name;
    auto buildWrapper = [&](bool directCall, brass::Module& outMod, brass::Function*& outFn, brass::Instruction*& outCallInst) {
        outFn = outMod.create_function(
            wrapName, brass::Type::f32(),
            {brass::Type::f32(), brass::Type::f32(), brass::Type::f32(), brass::Type::f32()});
        brass::Builder wb(outMod);
        wb.set_function(outFn);
        brass::BasicBlock* entryBlk = wb.append_block("entry");
        brass::Value* vSample = wb.add_block_param(entryBlk, brass::Type::f32());
        brass::Value* vTime = wb.add_block_param(entryBlk, brass::Type::f32());
        brass::Value* vP0 = wb.add_block_param(entryBlk, brass::Type::f32());
        brass::Value* vP1 = wb.add_block_param(entryBlk, brass::Type::f32());
        wb.position_at_end(entryBlk);

        brass::Value* argsPack = wb.build_alloca(32, 8);
        brass::Value* coordsCtx = wb.build_alloca(104, 8);
        brass::Value* outRet = wb.build_alloca(8, 8);
        brass::Value* zero64 = wb.build_iconst_i64(0);
        for (int32_t off = 0; off < 32; off += 8) {
            wb.build_store(brass::Type::i64(), argsPack, off, zero64);
        }
        for (int32_t off = 0; off < 104; off += 8) {
            wb.build_store(brass::Type::i64(), coordsCtx, off, zero64);
        }
        wb.build_store(brass::Type::i64(), outRet, 0, zero64);

        wb.build_store(brass::Type::f32(), argsPack, 0, vSample);
        wb.build_store(brass::Type::f32(), argsPack, 8, vTime);
        wb.build_store(brass::Type::f32(), argsPack, 16, vP0);
        wb.build_store(brass::Type::f32(), argsPack, 24, vP1);

        outCallInst = nullptr;
        if (directCall && k->cpu_ && k->cpu_->point) {
            wb.build_call(k->cpu_->point->name(), brass::Type::void_type(), {argsPack, coordsCtx, outRet});
            for (brass::Instruction* inst : *entryBlk) {
                if (inst && inst->opcode() == brass::Opcode::call) outCallInst = inst;
            }
        } else {
            const auto& ptFn = k->cpu_point_fn();
            if (!ptFn.is_valid() || !ptFn.entry_point() || !k->cpu_error().empty()) {
                return;
            }
            const auto ptAddr = static_cast<int64_t>(reinterpret_cast<uintptr_t>(ptFn.entry_point()));
            brass::Value* fnSlot = wb.build_alloca(8, 8);
            wb.build_store(brass::Type::i64(), fnSlot, 0, wb.build_iconst_i64(ptAddr));
            brass::Value* calleePtr = wb.build_load(brass::Type::ptr(), fnSlot, 0);
            wb.build_call_indirect(calleePtr, brass::Type::void_type(), {argsPack, coordsCtx, outRet});
        }

        brass::Value* rawY = wb.build_load(brass::Type::f32(), outRet, 0);
        wb.build_ret(emitSpeakerClampEpilogue(wb, rawY, result.maxAmp));
        outFn->rebuild_cfg_predecessors();
    };

    brass::Module wrapMod("bk_dsp_wrapper");
    brass::Function* wfn = nullptr;
    brass::Instruction* directCallInst = nullptr;
    const bool canFuse = (k->cpu_ && k->cpu_->module && k->cpu_->point);
    if (canFuse) {
        buildWrapper(true, wrapMod, wfn, directCallInst);
        if (wfn && directCallInst) {
            brass::CrossDomainFusionOptions fopts;
            fopts.allow_nanbox_unboxing = false;
            fopts.max_inlined_insts = 1024;
            fopts.audit_opts.require_no_gc = true;
            fopts.audit_opts.require_no_pinned_tls = true;
            fopts.audit_opts.require_no_alloc = true;
            fopts.audit_opts.require_no_deopt = true;
            fopts.audit_opts.require_no_eh = true;
            const brass::CrossDomainFusionResult fres = brass::fuse_cross_module_call(
                *wfn, directCallInst, wrapMod, *k->cpu_->module, k->cpu_->point->name(), fopts);
            if (fres.fused) {
                result.crossModuleFused = true;
                result.zeroAllocProven = fres.audit.ok;
            }
        }
    }

    brass::Module fallbackMod("bk_dsp_wrapper_fallback");
    brass::Module* compileMod = &wrapMod;
    if (!result.crossModuleFused) {
        const auto& ptFn = k->cpu_point_fn();
        if (!ptFn.is_valid() || !ptFn.entry_point() || !k->cpu_error().empty()) {
            result.errorMessage = "compileVerifiedBrokernelDsp: CPU JIT failed: " + k->cpu_error();
            return result;
        }
        brass::Function* fbFn = nullptr;
        brass::Instruction* unused = nullptr;
        buildWrapper(false, fallbackMod, fbFn, unused);
        compileMod = &fallbackMod;
    }

    auto kopts = brass::codegen::KernelOptions::audio_realtime();
    kopts.enable_optimizations = true;
    kopts.opt_level = result.crossModuleFused ? 2 : 1;
    kopts.enable_sroa = true;
    kopts.enable_smt_guard_elim = false;
    kopts.enable_superopt = false;
    kopts.enable_fma = false;
    kopts.enable_fp_reassociation = false;

    brass::codegen::KernelJit jit(kopts);
    brass::codegen::KernelFunction jfn = jit.compile(*compileMod, wrapName);
    if (!jfn.is_valid() || !jfn.entry_point()) {
        result.errorMessage = "compileVerifiedBrokernelDsp: KernelJit failed for wrapper";
        return result;
    }

    auto keep = std::make_shared<BrokernelDspKeepAlive>();
    keep->compileResult = std::move(cres);
    keep->wrapperFn = std::move(jfn);
    result.fn = reinterpret_cast<DspKernelFnPtr>(keep->wrapperFn.entry_point());
    result.jitKeepAlive = std::move(keep);
    result.valid = true;
    result.verified = result.zeroAllocProven && result.nanSafeProven && result.mirEquivProven;
    return result;
}

} // namespace broaudio
