// Compiled synthesis layers: the process-wide registry of layer shapes, their
// kernels, and the emitter. The emitter runs synth_dsp.h's templates with
// JitOps, whose values are brass IR, so the kernel is the interpreter's
// arithmetic operation for operation:
//
//   void kernel(const SynthKernelArgs* a, uint32_t* words, int32_t n)
//
// loads the layer's constants and delay-line pointers, computes the prep
// values, runs one loop over the frames carrying every state word in a
// register, accumulates out[i] + y * gain, and stores the words back.

#include "synth_dsp.h"

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#if BROAUDIO_HAS_JIT
#include <brass/codegen/audio_builder.hpp>
#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/types.hpp>
#endif

namespace broaudio {

// --- Registry ---------------------------------------------------------------

namespace {

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<SynthShape>> shapes;
    std::vector<std::shared_ptr<void>> kernels;   // kept for the process
};

// Leaked on purpose: shapes and kernels are referenced by voices and engines
// that may outlive static teardown.
Registry& registry()
{
    static Registry* r = new Registry();
    return *r;
}

std::atomic<uint32_t> g_wantGen{0};
std::atomic<uint32_t> g_handledGen{0};
std::mutex g_compileMutex;

} // namespace

const SynthShape* internSynthShape(LayerPlan&& plan)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.shapes.find(plan.key);
    if (it != r.shapes.end()) return it->second.get();
    auto shape = std::make_unique<SynthShape>();
    std::string key = plan.key;
    shape->plan = std::move(plan);
    const SynthShape* out = shape.get();
    r.shapes.emplace(std::move(key), std::move(shape));
    return out;
}

void requestSynthKernel(const SynthShape& shape) noexcept
{
    if (shape.entry.load(std::memory_order_relaxed) || shape.failed.load(std::memory_order_relaxed)) return;
    if (shape.wanted.load(std::memory_order_relaxed)) return;
    shape.wanted.store(true, std::memory_order_relaxed);
    g_wantGen.fetch_add(1, std::memory_order_release);
}

bool compileSynthKernelSync(const SynthShape& shape)
{
    if (shape.entry.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(g_compileMutex);
    if (shape.entry.load(std::memory_order_acquire)) return true;
    if (shape.failed.load(std::memory_order_relaxed)) return false;
    std::shared_ptr<void> keep;
    void* entry = nullptr;
    try {
        entry = buildSynthKernel(shape.plan, keep);
    } catch (...) {
        entry = nullptr;
    }
    if (!entry) {
        shape.failed.store(true, std::memory_order_relaxed);
        return false;
    }
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> rl(r.mutex);
        r.kernels.push_back(std::move(keep));
    }
    shape.entry.store(entry, std::memory_order_release);
    return true;
}

void compilePendingSynthKernels()
{
    const uint32_t gen = g_wantGen.load(std::memory_order_acquire);
    if (gen == g_handledGen.load(std::memory_order_relaxed)) return;
    std::vector<const SynthShape*> todo;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        for (auto& [key, s] : r.shapes) {
            if (s->wanted.load(std::memory_order_relaxed) && !s->entry.load(std::memory_order_relaxed) &&
                !s->failed.load(std::memory_order_relaxed))
                todo.push_back(s.get());
        }
    }
    for (const SynthShape* s : todo) compileSynthKernelSync(*s);
    g_handledGen.store(gen, std::memory_order_relaxed);
}

// --- Emitter ----------------------------------------------------------------

#if BROAUDIO_HAS_JIT

namespace {

using brass::BasicBlock;
using brass::Builder;
using brass::Function;
using brass::Module;
using brass::Type;
using brass::Value;

thread_local Builder* tlB = nullptr;
inline Builder& B() { return *tlB; }

struct JF { Value* v = nullptr; };
struct JI { Value* v = nullptr; };
struct JB { Value* v = nullptr; };

inline JF cf(float x) { return {B().build_fconst_f32(x)}; }

inline JF operator+(JF a, JF b) { return {B().build_add(a.v, b.v)}; }
inline JF operator-(JF a, JF b) { return {B().build_sub(a.v, b.v)}; }
inline JF operator*(JF a, JF b) { return {B().build_mul(a.v, b.v)}; }
inline JF operator/(JF a, JF b) { return {B().build_sdiv(a.v, b.v)}; }
inline JF operator-(JF a) { return {B().build_neg(a.v)}; }
inline JF operator+(JF a, float b) { return a + cf(b); }
inline JF operator-(JF a, float b) { return a - cf(b); }
inline JF operator*(JF a, float b) { return a * cf(b); }
inline JF operator/(JF a, float b) { return a / cf(b); }
inline JF operator+(float a, JF b) { return cf(a) + b; }
inline JF operator-(float a, JF b) { return cf(a) - b; }
inline JF operator*(float a, JF b) { return cf(a) * b; }
inline JF operator/(float a, JF b) { return cf(a) / b; }
// A double literal would make the interpreter compute in double.
JF operator+(JF, double) = delete;
JF operator-(JF, double) = delete;
JF operator*(JF, double) = delete;
JF operator/(JF, double) = delete;
JF operator+(double, JF) = delete;
JF operator-(double, JF) = delete;
JF operator*(double, JF) = delete;
JF operator/(double, JF) = delete;

inline JI operator+(JI a, JI b) { return {B().build_add(a.v, b.v)}; }
inline JI operator-(JI a, JI b) { return {B().build_sub(a.v, b.v)}; }
inline JI operator*(JI a, JI b) { return {B().build_mul(a.v, b.v)}; }
inline JI operator&(JI a, JI b) { return {B().build_and(a.v, b.v)}; }

struct JitOps {
    using F = JF;
    using I = JI;
    static F c(float v) { return cf(v); }
    static F floor(F x) { return {B().build_floor_f32(x.v)}; }
    static F abs(F x) { return {B().build_fabs_f32(x.v)}; }
    static F min(F a, F b) { return {B().build_fmin_f32(a.v, b.v)}; }
    static F max(F a, F b) { return {B().build_fmax_f32(a.v, b.v)}; }
    static JB lt(F a, F b) { return {B().build_slt(a.v, b.v)}; }
    static F sel(JB c, F a, F b) { return {B().build_select(c.v, a.v, b.v)}; }
    static I ic(int32_t v) { return {B().build_iconst_i32(v)}; }
    static I ashr8(I v) { return {B().build_ashr(v.v, B().build_iconst_i32(8))}; }
    static F itof(I v) { return {B().build_sitofp_f32_i32(v.v)}; }
    static F load(Value* p, I idx) { return {B().build_load_indexed(Type::f32(), p, idx.v, 4)}; }
    static void store(Value* p, I idx, F v) { B().build_store_indexed(Type::f32(), p, idx.v, 4, v.v); }
};

struct JitCx {
    std::vector<JF> kv;
    std::vector<JI> kiv;
    std::vector<Value*> bufv;
    std::vector<JF> fw;
    std::vector<JI> iw;
    std::vector<JF> prev;

    JF k(int i) const { return kv[i]; }
    JI ki(int i) const { return kiv[i]; }
    JF& f(int w) { return fw[w]; }
    JI& i(int w) { return iw[w]; }
    JF& pre(int j, int q) { return prev[static_cast<size_t>(j) * kSynthMaxPrep + q]; }
    Value* buf(int b) const { return bufv[b]; }
};

constexpr int32_t off(size_t o) { return static_cast<int32_t>(o); }

void emitLayer(Builder& b, const LayerPlan& plan, Value* A, Value* W, Value* N)
{
    tlB = &b;
    Value* out = b.build_load(Type::ptr(), A, off(offsetof(SynthKernelArgs, out)));
    Value* kp = b.build_load(Type::ptr(), A, off(offsetof(SynthKernelArgs, k)));
    JitCx cx;
    cx.kv.resize(plan.kCount);
    for (int i = 0; i < plan.kCount; ++i) cx.kv[i] = {b.build_load(Type::f32(), kp, 4 * i)};
    if (plan.kiCount > 0) {
        Value* kip = b.build_load(Type::ptr(), A, off(offsetof(SynthKernelArgs, ki)));
        cx.kiv.resize(plan.kiCount);
        for (int i = 0; i < plan.kiCount; ++i) cx.kiv[i] = {b.build_load(Type::i32(), kip, 4 * i)};
    }
    if (plan.bufCount > 0) {
        Value* bp = b.build_load(Type::ptr(), A, off(offsetof(SynthKernelArgs, bufs)));
        cx.bufv.resize(plan.bufCount);
        for (int i = 0; i < plan.bufCount; ++i)
            cx.bufv[i] = b.build_load(Type::ptr(), bp, off(i * sizeof(float*)));
    }
    cx.fw.resize(plan.words);
    cx.iw.resize(plan.words);
    cx.prev.resize(plan.nodes.size() * kSynthMaxPrep);
    for (size_t j = 0; j < plan.nodes.size(); ++j)
        synthdsp::prepNode<JitOps>(plan.nodes[j], static_cast<int>(j), cx);
    const JF gain = cx.k(LayerPlan::kGain);

    std::vector<Value*> init;
    for (int w = 0; w < plan.words; ++w)
        init.push_back(b.build_load(plan.wordIsInt[w] ? Type::i32() : Type::f32(), W, 4 * w));

    auto body = [&](Value* idx, const std::vector<Value*>& in) -> std::vector<Value*> {
        for (int w = 0; w < plan.words; ++w) {
            if (plan.wordIsInt[w]) cx.iw[w] = {in[w]};
            else cx.fw[w] = {in[w]};
        }
        JF y = synthdsp::layerFrame<JitOps>(plan, cx);
        JF o = JF{b.build_load_indexed(Type::f32(), out, idx, 4)} + y * gain;
        b.build_store_indexed(Type::f32(), out, idx, 4, o.v);
        std::vector<Value*> acc(plan.words);
        for (int w = 0; w < plan.words; ++w) acc[w] = plan.wordIsInt[w] ? cx.iw[w].v : cx.fw[w].v;
        return acc;
    };
    brass::codegen::AudioKernelBuilder kb(b);
    std::vector<Value*> fin = kb.for_range_reduce_n(b.build_iconst_i32(0), N, b.build_iconst_i32(1), init, body);
    for (int w = 0; w < plan.words; ++w)
        b.build_store(plan.wordIsInt[w] ? Type::i32() : Type::f32(), W, 4 * w, fin[w]);
    tlB = nullptr;
}

} // namespace

void* buildSynthKernel(const LayerPlan& plan, std::shared_ptr<void>& keepAlive)
{
    using namespace brass::codegen;
    Module mod("synth_layer_module");
    mod.set_allow_fp_reassociation(false);
    Function* fn = mod.create_function("synth_layer_kernel", Type::void_type(),
                                       {Type::ptr(), Type::ptr(), Type::i32()});
    AudioKernelBuilder kb(mod, fn);
    Builder& b = kb.builder();
    BasicBlock* entry = b.append_block("entry");
    Value* a0 = b.add_block_param(entry, Type::ptr());
    Value* a1 = b.add_block_param(entry, Type::ptr());
    Value* n = b.add_block_param(entry, Type::i32());
    kb.position_at_end(entry);
    emitLayer(b, plan, a0, a1, n);
    b.build_ret_void();

    KernelOptions opts = KernelOptions::audio_realtime();
    // Bit-exact with the interpreter: no contraction, no reassociation.
    opts.enable_fma = false;
    opts.enable_fp_reassociation = false;
    opts.enable_unroll = false;
    KernelJit jit(opts);
    KernelFunction kfn = jit.compile(mod, "synth_layer_kernel");
    if (!kfn.is_valid()) throw std::runtime_error("synth_jit: no entry point");
    auto k = std::make_shared<KernelFunction>(std::move(kfn));
    void* entryPoint = k->entry_point();
    keepAlive = k;
    return entryPoint;
}

#else // !BROAUDIO_HAS_JIT

void* buildSynthKernel(const LayerPlan&, std::shared_ptr<void>&)
{
    return nullptr;
}

#endif

} // namespace broaudio
