#pragma once
// Optional brokernel-compiled audio DSP block processor (built when BROAUDIO_WITH_BROKERNEL=ON).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace broaudio {

class KernelDsp {
public:
    KernelDsp();
    ~KernelDsp();
    KernelDsp(KernelDsp&&) noexcept;
    KernelDsp& operator=(KernelDsp&&) noexcept;
    KernelDsp(const KernelDsp&) = delete;
    KernelDsp& operator=(const KernelDsp&) = delete;

    /// Compiles a 1D `@kernel @grid(1)` `.bk` kernel (with `std::dsp` available).
    /// Supported signatures:
    ///   `fn k(g: grid<1>, buf: rw_store<f32, 1>)`
    ///   `fn k(g: grid<1>, buf: rw_store<f32, 1>, p0: f32)`
    ///   `fn k(g: grid<1>, buf: rw_store<f32, 1>, p0: f32, p1: f32)`
    bool compile(std::string_view bk_source,
                 std::string_view entry_name = "k_dsp",
                 std::string* err = nullptr);

    bool valid() const noexcept;

    /// Updates a runtime `@knob` scalar (`f32`) without recompilation.
    bool set_knob(std::string_view name, float value, std::string* err = nullptr);

    /// Runs the compiled kernel in-place on `samples[0..count)` via brokernel's CPU JIT.
    bool process_block(float* samples,
                       std::uint32_t count,
                       float p0 = 0.0f,
                       float p1 = 0.0f,
                       std::string* err = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace broaudio
