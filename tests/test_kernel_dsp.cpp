#include "broaudio/kernel_dsp.h"
#include "broaudio/verified_dsp_kernel.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)

int main() {
    const char* src = R"bk(
import std::dsp;

@knob
const GAIN: f32 = 1.0;

@kernel @grid(1)
fn k_dsp(g: grid<1>, buf: rw_store<f32, 1>, drive: f32) {
    let i = g.x();
    buf[i] = dsp::soft_clip(buf[i] * GAIN, drive);
}
)bk";

    broaudio::KernelDsp dsp;
    std::string err;
    CHECK(dsp.compile(src, "k_dsp", &err));
    CHECK(dsp.valid());

    std::vector<float> buf = {0.0f, 0.5f, -0.5f, 1.0f};
    CHECK(dsp.process_block(buf.data(), static_cast<std::uint32_t>(buf.size()), 2.0f, 0.0f, &err));
    CHECK(std::fabs(buf[0] - 0.0f) < 1e-5f);
    CHECK(std::fabs(buf[1] - (1.0f / 2.0f)) < 1e-5f);
    CHECK(std::fabs(buf[2] - (-1.0f / 2.0f)) < 1e-5f);
    CHECK(std::fabs(buf[3] - (2.0f / 3.0f)) < 1e-5f);

    // Runtime knob change without recompiling.
    CHECK(dsp.set_knob("GAIN", 0.5f, &err));
    std::vector<float> buf2 = {2.0f, -2.0f};
    CHECK(dsp.process_block(buf2.data(), 2, 1.0f, 0.0f, &err));
    CHECK(std::fabs(buf2[0] - 0.5f) < 1e-5f);
    CHECK(std::fabs(buf2[1] + 0.5f) < 1e-5f);

    // Verified point DSP kernel + SMT speaker protection + hotSwap integration.
    const char* ptSrc = R"bk(
import std::dsp;

pub fn bk_clip(sample: f32, time: f32, drive: f32, gain: f32) -> f32 {
    return (sample * gain + 0.0 * time) * (1.0 + drive) / (1.0 + drive * abs(sample));
}
)bk";

    broaudio::VerifiedDspCompileOptions vopts;
    vopts.language = "brokernel";
    vopts.entryName = "bk_clip";
    vopts.maxAmp = 0.75f;
    broaudio::CustomDspKernel vk = broaudio::compileVerifiedBrokernelDsp(ptSrc, vopts);
    if (!vk.valid || !vk.verified) {
        std::fprintf(stderr, "compileVerifiedBrokernelDsp failed: %s\n", vk.errorMessage.c_str());
    }
    CHECK(vk.valid);
    CHECK(vk.verified);
    CHECK(vk.zeroAllocProven);
    CHECK(vk.nanSafeProven);
    CHECK(vk.mirEquivProven);
    CHECK(vk.crossModuleFused);

    const float yNormal = vk.eval(0.25f, 0.0f, 1.0f, 1.0f);
    CHECK(std::fabs(yNormal - (0.5f / 1.25f)) < 1e-4f);

    // Speaker protection epilogue must clamp NaN, +Inf, -Inf, and large amplitudes to [-0.75, +0.75].
    const float yNan = vk.eval(std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 1.0f);
    const float yPosInf = vk.eval(std::numeric_limits<float>::infinity(), 0.0f, 1.0f, 1.0f);
    const float yNegInf = vk.eval(-std::numeric_limits<float>::infinity(), 0.0f, 1.0f, 1.0f);
    const float yBig = vk.eval(100.0f, 0.0f, 0.0f, 10.0f);
    CHECK(std::isfinite(yNan) && std::fabs(yNan) <= 0.75f);
    CHECK(std::isfinite(yPosInf) && std::fabs(yPosInf) <= 0.75f);
    CHECK(std::isfinite(yNegInf) && std::fabs(yNegInf) <= 0.75f);
    CHECK(std::fabs(yBig - 0.75f) < 1e-5f);

    broaudio::VerifiedDspProcessor proc;
    CHECK(proc.compileAndInstall("return sample * 0.5;", broaudio::VerifiedDspCompileOptions{}, &err));
    float blk[4] = {0.5f, -0.5f, 1.0f, 0.0f};
    proc.processBlock(blk, blk, 4, 0.0, 48000.0);
    CHECK(std::fabs(blk[0] - 0.25f) < 1e-5f);

    CHECK(proc.hotSwap(ptSrc, vopts, &err));
    proc.setParam(0, 1.0f);
    proc.setParam(1, 1.0f);
    float blk2[4] = {0.25f, 100.0f, std::numeric_limits<float>::quiet_NaN(), -0.25f};
    proc.processBlock(blk2, blk2, 4, 0.0, 48000.0);
    CHECK(std::fabs(blk2[0] - (0.5f / 1.25f)) < 1e-4f);
    CHECK(std::fabs(blk2[1] - 0.75f) < 1e-5f);
    CHECK(std::isfinite(blk2[2]) && std::fabs(blk2[2]) <= 0.75f);
    std::printf("test_kernel_dsp verified brokernel + hotSwap OK\n");
    return 0;
}
