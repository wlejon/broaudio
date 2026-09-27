// Synthesis graphs (synth/synth_graph.h): the compiled layers against the
// interpreter, bit for bit, the independence of the samples from the render's
// block sizes, seeded jitter, the end of a voice, note-off, parameter
// overrides and the description's validation messages.

#include "test_harness.h"
#include "synth_test_graphs.h"
#include "broaudio/spatial/voice_jit.h"
#include "broaudio/synth/synth_graph.h"

#include <cstring>
#include <string>

using namespace broaudio;

namespace {

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

size_t firstDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++)
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) return i;
    return n;
}

// Render one trigger in a repeating pattern of block sizes.
std::vector<float> renderPattern(const std::shared_ptr<const SynthGraph>& g, int sr, const SynthTrigger& t,
                                 bool compiled, const std::vector<int>& blocks, int maxFrames)
{
    SynthVoice v(g, sr, t);
    std::vector<float> out, tmp(4096);
    size_t b = 0;
    while (!v.finished() && static_cast<int>(out.size()) < maxFrames) {
        const int n = std::min(blocks[b++ % blocks.size()], maxFrames - static_cast<int>(out.size()));
        v.render(tmp.data(), n, compiled);
        out.insert(out.end(), tmp.begin(), tmp.begin() + n);
    }
    if (v.finished() && static_cast<int64_t>(out.size()) > v.endSample()) out.resize(v.endSample());
    return out;
}

float peakOf(const std::vector<float>& v)
{
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

const char* kGraphs[] = {synthtest::kGunshot, synthtest::kFmBell, synthtest::kMetalHit, synthtest::kKitchenSink};
const char* kNames[] = {"gunshot", "fm bell", "metal hit", "kitchen sink"};

} // namespace

TEST(parses_and_shares_shapes)
{
    auto a = SynthGraph::fromJson(synthtest::kMetalHit);
    auto b = SynthGraph::fromJson(synthtest::kMetalHit);
    ASSERT_EQ(a->layerCount(), 1);
    ASSERT_TRUE(a->layerShapeKey(0) == b->layerShapeKey(0));
    // The same wiring with other numbers and another declaration order is the same shape.
    auto c = SynthGraph::fromJson(R"({"nodes": {
        "res": {"type": "resonator", "input": "exc", "freq": 900, "modes": [{},{},{},{},{},{"ratio": 3}]},
        "ee": {"type": "env", "attack": 0.01, "decay": 0.02, "sustain": 0, "release": 0.001},
        "exc": {"type": "noise", "gain": "ee"}}, "output": "res"})");
    ASSERT_TRUE(c->layerShapeKey(0) == a->layerShapeKey(0));
    auto k = SynthGraph::fromJson(synthtest::kKitchenSink);
    ASSERT_EQ(k->layerCount(), 2);
    ASSERT_TRUE(k->paramIndex("b.offset") >= 0);
    ASSERT_TRUE(k->paramIndex("de.segments.1.level") >= 0);
    ASSERT_TRUE(k->paramIndex("vib.inputs.1.weight") >= 0);
    ASSERT_TRUE(k->paramIndex("nope") < 0);
    PASS();
}

TEST(compiled_matches_interpreted_bit_for_bit)
{
    if (!voiceJitBackendAvailable()) {
        std::printf("  (no brass backend: skipped)\n");
        PASS();
        return;
    }
    for (int gi = 0; gi < 4; gi++) {
        auto g = SynthGraph::fromJson(kGraphs[gi]);
        ASSERT_TRUE(g->precompile());
        for (int sr : {48000, 44100}) {
            for (uint32_t seed : {1u, 77u, 123456u}) {
                SynthTrigger t;
                t.seed = seed;
                const int maxFrames = sr * 4;
                auto ref = renderPattern(g, sr, t, false, {512}, maxFrames);
                auto jit = renderPattern(g, sr, t, true, {512}, maxFrames);
                if (!sameBits(ref, jit)) {
                    const size_t i = firstDiff(ref, jit);
                    std::printf("  %s @%d seed %u: %zu vs %zu frames, first diff at %zu: %.9g vs %.9g\n",
                                kNames[gi], sr, seed, ref.size(), jit.size(), i,
                                i < ref.size() ? ref[i] : 0.f, i < jit.size() ? jit[i] : 0.f);
                }
                ASSERT_TRUE(sameBits(ref, jit));
                ASSERT_GT(peakOf(ref), 0.01f);
            }
        }
    }
    PASS();
}

TEST(samples_do_not_depend_on_block_sizes)
{
    for (int gi = 0; gi < 4; gi++) {
        auto g = SynthGraph::fromJson(kGraphs[gi]);
        g->precompile();
        SynthTrigger t;
        t.seed = 9;
        const int sr = 48000, maxFrames = sr * 4;
        auto a = renderPattern(g, sr, t, false, {1024}, maxFrames);
        auto b = renderPattern(g, sr, t, false, {1, 37, 128, 500, 3, 2048, 64}, maxFrames);
        auto c = renderPattern(g, sr, t, true, {7, 256, 999, 128}, maxFrames);
        SynthRenderOptions o;
        o.sampleRate = sr;
        o.trigger = t;
        o.maxSeconds = 4.0;
        auto d = renderSynth(g, o);
        if (!sameBits(a, b)) std::printf("  %s: interpreted blocks differ at %zu\n", kNames[gi], firstDiff(a, b));
        ASSERT_TRUE(sameBits(a, b));
        ASSERT_TRUE(sameBits(a, c));
        ASSERT_TRUE(sameBits(a, d));
    }
    PASS();
}

TEST(seeds_vary_shots_deterministically)
{
    auto g = SynthGraph::fromJson(synthtest::kGunshot);
    SynthRenderOptions o;
    o.trigger.seed = 5;
    auto a = renderSynth(g, o);
    auto a2 = renderSynth(g, o);
    o.trigger.seed = 6;
    auto b = renderSynth(g, o);
    ASSERT_TRUE(sameBits(a, a2));
    ASSERT_FALSE(sameBits(a, b));

    // The jittered values: within their declared spread, repeatable.
    SynthTrigger t;
    t.seed = 5;
    SynthVoice v1(g, 48000, t), v2(g, 48000, t);
    const int di = g->paramIndex("e.decay");
    ASSERT_TRUE(di >= 0);
    ASSERT_TRUE(v1.values()[di] == v2.values()[di]);
    ASSERT_TRUE(v1.values()[di] != 0.03f);
    ASSERT_TRUE(v1.values()[di] >= 0.03f * 0.8f && v1.values()[di] <= 0.03f * 1.2f);
    int differ = 0;
    for (uint32_t s = 0; s < 64; s++) {
        SynthTrigger ts;
        ts.seed = s;
        SynthVoice vs(g, 48000, ts);
        const float d = vs.values()[di];
        ASSERT_TRUE(d >= 0.03f * 0.8f && d <= 0.03f * 1.2f);
        if (d != v1.values()[di]) differ++;
    }
    ASSERT_GT(differ, 60);
    // jitter off: declared values exactly.
    t.jitter = false;
    SynthVoice v3(g, 48000, t);
    ASSERT_TRUE(v3.values()[di] == 0.03f);
    PASS();
}

TEST(voices_end_and_durations_are_exact)
{
    auto g = SynthGraph::fromJson(synthtest::kGunshot);
    SynthRenderOptions o;
    auto a = renderSynth(g, o);
    ASSERT_GT(static_cast<int>(a.size()), 48000 / 10);
    ASSERT_TRUE(static_cast<int>(a.size()) < 48000);
    ASSERT_TRUE(a.size() % 256 == 0);   // ends on a tail window
    // The kitchen sink's envelopes and comb die out before its 1.2 s cap.
    auto k = SynthGraph::fromJson(synthtest::kKitchenSink);
    auto b = renderSynth(k, o);
    ASSERT_TRUE(b.size() < 57600 && b.size() > 48000 / 2);
    // A duration cuts a voice exactly; without an amplitude envelope or a
    // duration a voice runs until the caller's cap.
    auto capped = SynthGraph::fromJson(R"({"nodes": {"o": {"type": "osc"}}, "output": "o", "duration": 0.25})");
    ASSERT_EQ(static_cast<int>(renderSynth(capped, o).size()), 12000);
    auto drone = SynthGraph::fromJson(R"({"nodes": {"o": {"type": "osc"}}, "output": "o"})");
    o.maxSeconds = 0.5;
    ASSERT_EQ(static_cast<int>(renderSynth(drone, o).size()), 24000);
    PASS();
}

TEST(sustain_holds_until_release)
{
    auto g = SynthGraph::fromJson(R"({"nodes": {
        "o": {"type": "osc", "freq": 300, "gain": "e"},
        "e": {"type": "env", "attack": 0.01, "decay": 0.05, "sustain": 0.5, "release": 0.1}}, "output": "o"})");
    SynthVoice v(g, 48000, SynthTrigger{});
    std::vector<float> buf(4800);
    for (int b = 0; b < 20; b++) v.render(buf.data(), 4800, false);   // 2 s held
    ASSERT_FALSE(v.finished());
    ASSERT_NEAR(peakOf(buf), 0.5f, 0.01f);
    v.release();
    int blocks = 0;
    while (!v.finished() && blocks < 50) { v.render(buf.data(), 4800, false); blocks++; }
    ASSERT_TRUE(v.finished());
    ASSERT_TRUE(blocks <= 3);
    // gate: the same envelope releases by itself.
    auto gated = SynthGraph::fromJson(R"({"nodes": {
        "o": {"type": "osc", "freq": 300, "gain": "e"},
        "e": {"type": "env", "attack": 0.01, "decay": 0.05, "sustain": 0.5, "release": 0.1, "gate": 0.3}},
        "output": "o"})");
    auto r = renderSynth(gated, SynthRenderOptions{});
    ASSERT_GT(static_cast<int>(r.size()), 48000 * 4 / 10);
    ASSERT_TRUE(static_cast<int>(r.size()) < 48000 * 6 / 10);
    PASS();
}

TEST(overrides_replace_declared_values)
{
    auto g = SynthGraph::fromJson(synthtest::kMetalHit);
    auto ov = g->overridesFromJson(R"({"res.freq": 1040, "res.modes.0.decay": 0.3})");
    ASSERT_EQ(static_cast<int>(ov.size()), 2);
    SynthTrigger t;
    t.jitter = false;
    t.overrides = ov;
    SynthVoice v(g, 48000, t);
    ASSERT_TRUE(v.values()[g->paramIndex("res.freq")] == 1040.0f);
    bool threw = false;
    try { g->overridesFromJson(R"({"res.fraq": 1})"); } catch (const SynthGraphError& e) {
        threw = std::string(e.what()).find("res.fraq") != std::string::npos;
    }
    ASSERT_TRUE(threw);
    threw = false;
    try { g->overridesFromJson(R"({"res.freq": 0})"); } catch (const SynthGraphError& e) { threw = e.isRange(); }
    ASSERT_TRUE(threw);
    PASS();
}

TEST(invalid_descriptions_name_the_field)
{
    struct Bad { const char* json; const char* expect; };
    const Bad bad[] = {
        {R"([1,2])", "must be an object"},
        {R"({"nodes": {"a": {"type": "osc"}}})", "output: required"},
        {R"({"nodes": {"a": {"type": "noize"}}, "output": "a"})", "nodes.a.type: unknown type 'noize'"},
        {R"({"nodes": {"a": {"type": "osc", "frq": 3}}, "output": "a"})", "unknown field 'frq'"},
        {R"({"nodes": {"a": {"type": "osc", "wave": "sawtooth"}}, "output": "a"})", "nodes.a.wave: unknown value 'sawtooth'"},
        {R"({"nodes": {"a": {"type": "osc", "freq": "b"}}, "output": "a"})", "nodes.a.freq: unknown node 'b'"},
        {R"({"nodes": {"a": {"type": "filter"}}, "output": "a"})", "nodes.a.input: required"},
        {R"({"nodes": {"a": {"type": "mul", "a": "b", "b": 1}, "b": {"type": "mul", "a": "a", "b": 1}}, "output": "a"})", "cycle: a -> b -> a"},
        {R"({"nodes": {"a": {"type": "osc"}, "b": {"type": "osc"}}, "output": "a"})", "nodes.b: node 'b' does not reach the output"},
        {R"({"nodes": {"a": {"type": "filter", "input": "n", "q": 0}, "n": {"type": "noise"}}, "output": "a"})", "nodes.a.q: 0 is out of range"},
        {R"({"nodes": {"a": {"type": "osc", "freq": {"value": 3, "jiter": 1}}}, "output": "a"})", "unknown field 'jiter'"},
        {R"({"nodes": {"a": {"type": "osc", "freq": {"value": 3, "jitter": 2}}}, "output": "a"})", "nodes.a.freq.jitter"},
        {R"({"nodes": {"a": {"type": "resonator", "input": "n", "freq": "n"}, "n": {"type": "noise"}}, "output": "a"})", "nodes.a.freq: must be a number"},
        {R"({"layers": {"x": {"nodes": {"a": {"type": "osc"}}, "output": "a"}, "y": {"nodes": {"b": {"type": "osc", "freq": "a"}}, "output": "b"}}})", "in another layer"},
        {R"({"nodes": {"a": {"type": "env", "segments": [{"time": 1, "level": 1}], "hold": 3}}, "output": "a"})", "nodes.a.hold"},
        {R"({"nodes": {"a": {"type": "sweep", "from": 1}}, "output": "a"})", "nodes.a.to: required"},
    };
    for (const Bad& b : bad) {
        std::string msg;
        try {
            SynthGraph::fromJson(b.json);
        } catch (const SynthGraphError& e) {
            msg = e.what();
        }
        if (msg.find(b.expect) == std::string::npos)
            std::printf("  expected '%s', got '%s'\n", b.expect, msg.c_str());
        ASSERT_TRUE(msg.find(b.expect) != std::string::npos);
    }
    PASS();
}

int main() { return runAllTests(); }
