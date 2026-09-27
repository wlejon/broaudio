// The interpreted synthesis layer: synth_dsp.h over plain floats. This is the
// reference the compiled kernels (synth_jit.cpp) reproduce bit for bit.

#include "synth_dsp.h"

#include <cmath>
#include <cstring>

namespace broaudio {

namespace {

struct InterpOps {
    using F = float;
    using I = uint32_t;   // two's-complement wrap, as the kernel's i32
    static F c(float v) { return v; }
    static F floor(F x) { return std::floor(x); }
    static F abs(F x) { return std::fabs(x); }
    // minss / maxss: the first operand when the comparison holds, else the second.
    static F min(F a, F b) { return a < b ? a : b; }
    static F max(F a, F b) { return a > b ? a : b; }
    static bool lt(F a, F b) { return a < b; }
    static F sel(bool c, F a, F b) { return c ? a : b; }
    static I ic(int32_t v) { return static_cast<I>(v); }
    static I ashr8(I v) { return static_cast<I>(static_cast<int32_t>(v) >> 8); }
    static F itof(I v) { return static_cast<float>(static_cast<int32_t>(v)); }
    static F load(float* p, I idx) { return p[idx]; }
    static void store(float* p, I idx, F v) { p[idx] = v; }
};

struct InterpCx {
    const float* kp;
    const int32_t* kip;
    float* const* bufs;
    float fw[kSynthMaxWords];
    uint32_t iw[kSynthMaxWords];
    float pre_[kSynthMaxNodes][kSynthMaxPrep];

    float k(int i) const { return kp[i]; }
    uint32_t ki(int i) const { return static_cast<uint32_t>(kip[i]); }
    float& f(int w) { return fw[w]; }
    uint32_t& i(int w) { return iw[w]; }
    float& pre(int j, int q) { return pre_[j][q]; }
    float* buf(int b) const { return bufs[b]; }
};

} // namespace

void runLayerInterpreted(const LayerPlan& plan, const SynthKernelArgs& a, uint32_t* words, int n)
{
    if (n <= 0) return;
    InterpCx cx;
    cx.kp = a.k;
    cx.kip = a.ki;
    cx.bufs = a.bufs;
    for (int w = 0; w < plan.words; ++w) {
        if (plan.wordIsInt[w]) cx.iw[w] = words[w];
        else std::memcpy(&cx.fw[w], &words[w], sizeof(float));
    }
    for (size_t j = 0; j < plan.nodes.size(); ++j)
        synthdsp::prepNode<InterpOps>(plan.nodes[j], static_cast<int>(j), cx);
    const float gain = a.k[LayerPlan::kGain];
    float* out = a.out;
    for (int i = 0; i < n; ++i) {
        const float y = synthdsp::layerFrame<InterpOps>(plan, cx);
        out[i] = out[i] + y * gain;
    }
    for (int w = 0; w < plan.words; ++w) {
        if (plan.wordIsInt[w]) words[w] = cx.iw[w];
        else std::memcpy(&words[w], &cx.fw[w], sizeof(float));
    }
}

} // namespace broaudio
