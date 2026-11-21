#include "broaudio/kernel_dsp.h"

#include <brokernel/brokernel.h>

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

} // namespace broaudio
