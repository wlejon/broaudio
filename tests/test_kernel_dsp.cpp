#include "broaudio/kernel_dsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    return 0;
}
