// The impulses node (synthesis graphs' random-trigger source): the compiled
// kernel against the interpreter, bit for bit, for every grain shape, with a
// constant and a wired rate, used as a source, a gate and an exciter; block
// size independence; the event statistics (periodic at jitter 0, the mean
// rate kept under jitter, amplitude variation within its bound, grain
// lengths); seeded determinism across seeds and threads; its parameters and
// validation messages.

#include "test_harness.h"
#include "broaudio/spatial/voice_jit.h"
#include "broaudio/synth/synth_graph.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>

using namespace broaudio;

namespace {

// A rattle: decaying clicks at a jittered rate ringing two modes, under a
// decay envelope.
const char* kRattle = R"({"nodes": {
    "r":   {"type": "impulses", "shape": "decay", "rate": {"value": 18, "jitter": 0.1}, "jitter": 0.6,
            "length": 0.003, "ampJitter": 0.5},
    "res": {"type": "resonator", "input": "r", "freq": 1800, "modes": [
              {"ratio": 1, "decay": 0.05, "gain": 0.3}, {"ratio": 2.3, "decay": 0.03, "gain": 0.2}]},
    "e":   {"type": "env", "attack": 0.001, "decay": 0.6, "sustain": 0, "release": 0.05},
    "out": {"type": "mul", "a": "res", "b": "e"}
  }, "output": "out"})";

// Track links slowing down (a wired rate): Hann grains gating pink noise;
// and clanks: random impulses into a comb, gated by rectangular grains.
const char* kTrack = R"({"layers": {
  "links": {"nodes": {
      "sp": {"type": "sweep", "from": 30, "to": 8, "time": 0.5, "curve": "exp"},
      "g":  {"type": "impulses", "shape": "hann", "rate": "sp", "jitter": 0.2, "length": 0.012, "ampJitter": 0.3},
      "n":  {"type": "noise", "color": "pink"},
      "m":  {"type": "mul", "a": "n", "b": "g"},
      "bp": {"type": "filter", "mode": "bandpass", "input": "m", "cutoff": 900, "q": 2}
    }, "output": "bp"},
  "clank": {"nodes": {
      "k":  {"type": "impulses", "rate": 6, "jitter": 1, "ampJitter": 1},
      "cb": {"type": "comb", "input": "k", "freq": 320, "feedback": 0.9, "damp": 0.2},
      "gt": {"type": "impulses", "shape": "rect", "rate": 3, "length": 0.08},
      "o":  {"type": "mul", "a": "cb", "b": "gt"}
    }, "output": "o"}
}, "duration": 1.0})";

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

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

std::vector<float> renderOne(const char* desc, uint32_t seed = 1, int sr = 48000, double seconds = 1.0)
{
    SynthRenderOptions o;
    o.sampleRate = sr;
    o.trigger.seed = seed;
    o.maxSeconds = seconds;
    return renderSynth(SynthGraph::fromJson(desc), o);
}

std::vector<int> eventsOf(const std::vector<float>& v)
{
    std::vector<int> at;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] != 0.0f) at.push_back(static_cast<int>(i));
    return at;
}

float peakOf(const std::vector<float>& v)
{
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

} // namespace

TEST(compiled_matches_interpreted_bit_for_bit)
{
    const bool jit = voiceJitBackendAvailable();
    const char* graphs[] = {kRattle, kTrack};
    for (const char* desc : graphs) {
        auto g = SynthGraph::fromJson(desc);
        if (jit) ASSERT_TRUE(g->precompile());
        for (int sr : {48000, 44100}) {
            for (uint32_t seed : {1u, 77u, 123456u}) {
                SynthTrigger t;
                t.seed = seed;
                const int maxFrames = sr * 2;
                auto ref = renderPattern(g, sr, t, false, {512}, maxFrames);
                auto blocks = renderPattern(g, sr, t, false, {1, 37, 128, 500, 3, 2048}, maxFrames);
                ASSERT_TRUE(sameBits(ref, blocks));
                ASSERT_GT(peakOf(ref), 0.01f);
                if (!jit) continue;
                auto comp = renderPattern(g, sr, t, true, {7, 256, 999, 128}, maxFrames);
                ASSERT_TRUE(sameBits(ref, comp));
            }
        }
    }
    if (!jit) std::printf("  (no brass backend: interpreter only)\n");
    PASS();
}

TEST(periodic_at_zero_jitter)
{
    // One unit impulse every 480 frames (the phase accumulates in float, so
    // an event can land a frame early or late), the first on frame 0.
    const auto v = renderOne(R"({"nodes": {"i": {"type": "impulses", "rate": 100}}, "output": "i", "duration": 1})");
    const auto at = eventsOf(v);
    ASSERT_TRUE(at.size() >= 99 && at.size() <= 101);
    ASSERT_EQ(at[0], 0);
    for (size_t k = 1; k < at.size(); k++) {
        ASSERT_TRUE(at[k] - at[k - 1] >= 479 && at[k] - at[k - 1] <= 481);
        ASSERT_TRUE(v[at[k]] == 1.0f);
    }
    PASS();
}

TEST(jitter_keeps_the_mean_rate_and_bounds_the_intervals)
{
    const auto v = renderOne(R"({"nodes": {"i": {"type": "impulses", "rate": 100, "jitter": 0.5,
        "ampJitter": 0.4}}, "output": "i", "duration": 4})", 3, 48000, 4.0);
    const auto at = eventsOf(v);
    ASSERT_TRUE(at.size() >= 370 && at.size() <= 430);
    int lo = 1 << 30, hi = 0;
    for (size_t k = 1; k < at.size(); k++) {
        lo = std::min(lo, at[k] - at[k - 1]);
        hi = std::max(hi, at[k] - at[k - 1]);
    }
    // Thresholds in [0.5, 1.5) of the mean interval (480 frames).
    ASSERT_TRUE(lo >= 238 && lo < 330);
    ASSERT_TRUE(hi <= 722 && hi > 630);
    float amin = 2, amax = 0;
    for (int i : at) {
        amin = std::min(amin, v[i]);
        amax = std::max(amax, v[i]);
    }
    ASSERT_TRUE(amin >= 0.6f && amin < 0.65f);
    ASSERT_TRUE(amax <= 1.0f && amax > 0.95f);
    PASS();
}

TEST(grain_shapes)
{
    // Rect: 0.01 s at full level, then 0 until the next event.
    auto rect = renderOne(R"({"nodes": {"i": {"type": "impulses", "shape": "rect", "rate": 10, "length": 0.01}},
        "output": "i", "duration": 0.2})");
    int on = 0;
    for (int i = 0; i < 4800; i++) on += rect[i] == 1.0f;
    ASSERT_TRUE(on >= 479 && on <= 481);
    for (int i = 482; i < 4790; i++) ASSERT_TRUE(rect[i] == 0.0f);
    // Hann: 0 at the grain's start, the level at its middle, 0 after it.
    auto hann = renderOne(R"({"nodes": {"i": {"type": "impulses", "shape": "hann", "rate": 10, "length": 0.01}},
        "output": "i", "duration": 0.2})");
    ASSERT_TRUE(hann[0] == 0.0f);
    ASSERT_NEAR(hann[240], 1.0f, 1e-4f);
    ASSERT_NEAR(hann[120], 0.5f, 0.01f);
    for (int i = 482; i < 4790; i++) ASSERT_TRUE(hann[i] == 0.0f);
    // Decay: 60 dB over the length.
    auto dec = renderOne(R"({"nodes": {"i": {"type": "impulses", "shape": "decay", "rate": 10, "length": 0.01}},
        "output": "i", "duration": 0.2})");
    ASSERT_TRUE(dec[0] == 1.0f);
    ASSERT_NEAR(dec[480], 0.001f, 1e-4f);
    bool next = false;
    for (int i = 4798; i <= 4802; i++) next = next || dec[i] == 1.0f;
    ASSERT_TRUE(next && dec[4790] < dec[480]);
    PASS();
}

TEST(seeded_and_deterministic_across_threads)
{
    auto a = renderOne(kRattle, 5), a2 = renderOne(kRattle, 5), b = renderOne(kRattle, 6);
    ASSERT_TRUE(sameBits(a, a2));
    ASSERT_FALSE(sameBits(a, b));
    // Event times differ between seeds (the node's own stream).
    const char* bare = R"({"nodes": {"i": {"type": "impulses", "rate": 50, "jitter": 1}}, "output": "i", "duration": 1})";
    ASSERT_FALSE(eventsOf(renderOne(bare, 1)) == eventsOf(renderOne(bare, 2)));
    std::vector<std::vector<float>> outs(4);
    std::vector<std::thread> ts;
    for (int k = 0; k < 4; k++) ts.emplace_back([&, k] { outs[k] = renderOne(kTrack, 9); });
    for (auto& t : ts) t.join();
    const auto ref = renderOne(kTrack, 9);
    for (const auto& o : outs) ASSERT_TRUE(sameBits(o, ref));
    PASS();
}

TEST(parameters_and_validation)
{
    auto g = SynthGraph::fromJson(kRattle);
    for (const char* name : {"r.rate", "r.jitter", "r.length", "r.ampJitter", "r.gain"})
        ASSERT_TRUE(g->paramIndex(name) >= 0);
    // An override moves the rate: twice the events.
    auto bare = SynthGraph::fromJson(R"({"nodes": {"i": {"type": "impulses", "rate": 20}}, "output": "i", "duration": 1})");
    SynthRenderOptions o;
    o.trigger.overrides = bare->overridesFromJson(R"({"i.rate": 40})");
    const size_t events = eventsOf(renderSynth(bare, o)).size();
    ASSERT_TRUE(events == 40 || events == 41);
    // Each shape is its own kernel shape.
    auto h = SynthGraph::fromJson(R"({"nodes": {"i": {"type": "impulses", "shape": "hann"}}, "output": "i"})");
    auto d = SynthGraph::fromJson(R"({"nodes": {"i": {"type": "impulses", "shape": "decay"}}, "output": "i"})");
    ASSERT_FALSE(h->layerShapeKey(0) == d->layerShapeKey(0));

    struct Bad { const char* json; const char* expect; };
    const Bad bad[] = {
        {R"({"nodes": {"i": {"type": "impulses", "shape": "saw"}}, "output": "i"})", "nodes.i.shape: unknown value 'saw'"},
        {R"({"nodes": {"i": {"type": "impulses", "jitter": 2}}, "output": "i"})", "nodes.i.jitter: 2 is out of range"},
        {R"({"nodes": {"i": {"type": "impulses", "length": 0}}, "output": "i"})", "nodes.i.length: 0 is out of range"},
        {R"({"nodes": {"i": {"type": "impulses", "length": "e"}, "e": {"type": "env"}}, "output": "i"})", "nodes.i.length: must be a number"},
        {R"({"nodes": {"i": {"type": "impulses", "density": 3}}, "output": "i"})", "unknown field 'density'"},
    };
    for (const Bad& b : bad) {
        std::string msg;
        try {
            SynthGraph::fromJson(b.json);
        } catch (const SynthGraphError& e) {
            msg = e.what();
        }
        if (msg.find(b.expect) == std::string::npos) std::printf("  expected '%s', got '%s'\n", b.expect, msg.c_str());
        ASSERT_TRUE(msg.find(b.expect) != std::string::npos);
    }
    PASS();
}

int main() { return runAllTests(); }
