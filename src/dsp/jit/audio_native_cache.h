#pragma once

#include <brass/cache/artifact_cache.hpp>
#include <brass/cache/native_module_cache.hpp>
#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/module.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace broaudio::detail {

inline std::filesystem::path audio_cache_directory()
{
    if (const char* env_dir = std::getenv("BROAUDIO_CACHE_DIR"); env_dir && env_dir[0] != '\0') {
        return std::filesystem::path(env_dir);
    }
#if defined(_WIN32)
    if (const char* local_app = std::getenv("LOCALAPPDATA"); local_app && local_app[0] != '\0') {
        return std::filesystem::path(local_app) / "broaudio" / "kernels";
    }
    if (const char* user_prof = std::getenv("USERPROFILE"); user_prof && user_prof[0] != '\0') {
        return std::filesystem::path(user_prof) / ".cache" / "broaudio" / "kernels";
    }
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "broaudio" / "kernels";
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::filesystem::path(home) / ".cache" / "broaudio" / "kernels";
    }
#endif
    std::error_code ec;
    auto tmp = std::filesystem::temp_directory_path(ec);
    if (!ec && !tmp.empty()) {
        return tmp / "broaudio" / "kernels";
    }
    return std::filesystem::path(".cache") / "broaudio" / "kernels";
}

inline brass::cache::NativeModuleCache& audio_native_module_cache()
{
    static constexpr uint64_t kMaxBytes = 64ULL * 1024 * 1024;
    // Leaked on purpose so cached NativeModule loads/handles safely outlive static teardown.
    static auto* store = new brass::cache::ArtifactCache(audio_cache_directory(), kMaxBytes);
    static auto* cache = new brass::cache::NativeModuleCache(*store);
    return *cache;
}

inline void hash_kernel_options(brass::cache::KeyBuilder& kb, const brass::codegen::KernelOptions& opts)
{
    kb.add_u64("opt_level", static_cast<uint64_t>(opts.opt_level));
    kb.add_bool("enable_optimizations", opts.enable_optimizations);
    kb.add_bool("enable_sroa", opts.enable_sroa);
    kb.add_bool("enable_bce", opts.enable_bce);
    kb.add_bool("enable_smt_guard_elim", opts.enable_smt_guard_elim);
    kb.add_bool("enable_superopt", opts.enable_superopt);
    kb.add_bool("enable_avx2", opts.enable_avx2);
    kb.add_bool("enable_fma", opts.enable_fma);
    kb.add_bool("enable_fp_reassociation", opts.enable_fp_reassociation);
    kb.add_bool("enable_vectorize", opts.enable_vectorize);
    kb.add_bool("enable_slp", opts.enable_slp);
    kb.add_bool("enable_unroll", opts.enable_unroll);
    kb.add_u64("unroll_factor", static_cast<uint64_t>(opts.unroll_factor));
    kb.add_bool("enable_parallel", opts.enable_parallel);
    kb.add_u64("parallel_threshold", opts.parallel_threshold);
    kb.add_u64("parallel_workers", static_cast<uint64_t>(opts.parallel_workers));
    kb.add_bool("enable_ftz_daz", opts.enable_ftz_daz);
}

template <typename BuildMirFn>
inline brass::codegen::KernelFunction compile_cached_audio_kernel(
    const brass::cache::CacheKey& client_key,
    const char* module_name,
    const char* entry_name,
    const brass::codegen::KernelOptions& opts,
    BuildMirFn&& build_mir)
{
    using namespace brass::codegen;

    const brass::cache::NativeOptions native_opts;
    std::string cache_err;
    std::shared_ptr<const brass::cache::NativeModule> native_mod =
        audio_native_module_cache().get_or_build_object(
            client_key,
            [&](brass::object::ObjectFile& obj, std::string&) -> bool {
                brass::Module mod(module_name);
                build_mir(mod);
                KernelJit jit(opts);
                obj = jit.compile_object(mod);
                return true;
            },
            native_opts,
            &cache_err);

    if (native_mod) {
        if (void* sym = native_mod->symbol(entry_name)) {
            return KernelFunction::from_owner(entry_name, sym, 0, std::move(native_mod));
        }
    }

    // Fallback to in-process KernelJit if disk caching or AOT linking failed.
    brass::Module mod(module_name);
    build_mir(mod);
    KernelJit jit(opts);
    return jit.compile(mod, entry_name);
}

} // namespace broaudio::detail
