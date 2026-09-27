// Cost of synthesis voices, interpreted vs compiled (synth/synth_graph.h), at
// 100 voices of one graph: ns per voice per 128-frame block at 48 kHz, and
// the share of the block's real-time budget all 100 take. The source stage
// alone (the voice's render); the distance chain after it is measured by
// test_voice_jit_cost. Each voice has its own seed. Both paths' outputs are
// compared, so the bench cannot time a kernel that went wrong.

#include "test_harness.h"
#include "synth_test_graphs.h"
#include "broaudio/spatial/voice_jit.h"
#include "broaudio/synth/synth_graph.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace broaudio;

namespace {

constexpr int kBlock = 128;
constexpr int kSr = 48000;
constexpr int kVoices = 100;
constexpr int kBlocks = 80;    // 0.21 s: every voice is still sounding (the gunshot ends at ~0.26 s)

using Clock = std::chrono::steady_clock;

// ns per voice per block, and the summed output of all voices.
double timeVoices(const std::shared_ptr<const SynthGraph>& g, bool compiled, std::vector<float>& sum, int& alive)
{
    std::vector<std::unique_ptr<SynthVoice>> voices;
    for (int v = 0; v < kVoices; v++) {
        SynthTrigger t;
        t.seed = 1000u + static_cast<uint32_t>(v);
        voices.push_back(std::make_unique<SynthVoice>(g, kSr, t));
    }
    std::vector<float> out(kBlock * kVoices);
    sum.assign(static_cast<size_t>(kBlock) * kBlocks, 0.0f);
    const auto t0 = Clock::now();
    for (int b = 0; b < kBlocks; b++) {
        for (int v = 0; v < kVoices; v++) voices[v]->render(out.data() + v * kBlock, kBlock, compiled);
        for (int v = 0; v < kVoices; v++)
            for (int i = 0; i < kBlock; i++) sum[b * kBlock + i] += out[v * kBlock + i];
    }
    const auto d = Clock::now() - t0;
    alive = 0;
    for (auto& v : voices) alive += v->finished() ? 0 : 1;
    // The summing loop is a few ns per voice-block; left in, it is the same
    // for both paths.
    return std::chrono::duration<double, std::nano>(d).count() / (double(kVoices) * kBlocks);
}

double rtPercent(double nsPerVoice, int voices)
{
    const double budgetNs = 1e9 * kBlock / kSr;
    return 100.0 * nsPerVoice * voices / budgetNs;
}

} // namespace

TEST(interpreted_vs_compiled_at_100_voices)
{
    struct Case { const char* name; const char* json; };
    const Case cases[] = {
        {"gunshot (noise burst + swept body)", synthtest::kGunshot},
        {"fm bell (2-op, index env)", synthtest::kFmBell},
        {"metal hit (6-mode resonator)", synthtest::kMetalHit},
        {"kitchen sink (every node kind)", synthtest::kKitchenSink},
    };
    const bool jit = voiceJitBackendAvailable();
    std::printf("  %-36s %6s %11s %11s %9s %9s %8s\n", "graph", "voices", "interp ns", "jit ns", "interp RT",
                "jit RT", "speedup");
    for (const Case& c : cases) {
        auto g = SynthGraph::fromJson(c.json);
        if (jit) ASSERT_TRUE(g->precompile());
        std::vector<float> sumI, sumJ, warm;
        int aliveI = 0, aliveJ = 0, w = 0;
        timeVoices(g, jit, warm, w);   // warm caches and the allocator
        const double ni = timeVoices(g, false, sumI, aliveI);
        const double nj = jit ? timeVoices(g, true, sumJ, aliveJ) : ni;
        std::printf("  %-36s %6d %11.1f %11.1f %8.2f%% %8.2f%% %7.2fx\n", c.name, kVoices, ni, nj,
                    rtPercent(ni, kVoices), rtPercent(nj, kVoices), ni / nj);
        ASSERT_EQ(aliveI, kVoices);
        if (jit) ASSERT_TRUE(std::memcmp(sumI.data(), sumJ.data(), sumI.size() * sizeof(float)) == 0);
    }
    PASS();
}

int main() { return runAllTests(); }
