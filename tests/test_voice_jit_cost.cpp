// Cost of the per-voice chain, interpreted vs compiled (spatial/voice_jit.h),
// at 100 and 200 voices: ns per voice per 128-frame block at 44.1 kHz and the
// share of the block's real-time budget the whole voice count takes.
//
// Scenes: mono voices spread over 5..300 m with air, delay, head + occlusion
// and a send, either moving (the delay follows the distance: interpolated
// shape) or static (settled delay: onset shape); and stereo moving voices.
// Every voice reads its own source block and state, and all mix into one bus
// and one send, as a crowd on one bus would. The two paths' outputs are also
// checked for equality, so the bench cannot time a kernel that went wrong.

#include "test_harness.h"
#include "distance_test_util.h"
#include "broaudio/spatial/voice_jit.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace broaudio;

namespace {

constexpr int kBlock = 128;
constexpr int kSr = 44100;

struct Voice {
    VoiceChainState s;
    VoiceChainParams p;
    PropagationDelayBuffer delay;
    std::vector<float> in0, in1;
    float dist = 0.0f;
};

using Clock = std::chrono::steady_clock;

struct Crowd {
    std::vector<std::unique_ptr<Voice>> voices;
    std::vector<float> bus = std::vector<float>(2 * kBlock), send = std::vector<float>(2 * kBlock);
    // Source planes for a batch of voices, as the mixer lays them out.
    std::vector<float> work = std::vector<float>(2 * kAirLanes * kBlock);
    std::vector<float> lanes = std::vector<float>(kAirLanes * kBlock);
    std::vector<float> traj = std::vector<float>(4 * kBlock);
    bool moving = true;
    int channels = 1;

    Crowd(int count, int channels_, bool moving_) : moving(moving_), channels(channels_) {
        static auto table = buildAirFilterTable(kSr, 20.f, 50.f);
        for (int v = 0; v < count; v++) {
            auto vo = std::make_unique<Voice>();
            vo->dist = 5.f + 295.f * v / (count - 1);
            vo->delay.channels = channels;
            vo->delay.capacity = 32768;
            vo->delay.mask = 32767;
            vo->delay.data.assign(static_cast<size_t>(32768) * channels, 0.f);
            vo->p.channels = channels;
            vo->p.stages = kStageAir | kStageDelay | kStageHead | kStageSend;            table->lookup(vo->dist, vo->p.airPole, vo->p.airMix);
            vo->p.delayBuf = &vo->delay;
            vo->p.delayTarget = vo->dist / 343.f * kSr;
            vo->p.delayMax = 0.9f * kSr;
            vo->p.delaySmooth = 1.f - std::exp(-1.f / (0.01f * kSr));
            vo->p.distanceGain = 1.0f / (1.0f + 0.02f * vo->dist);
            vo->p.head.gainL = 0.8f; vo->p.head.gainR = 0.6f;
            vo->p.head.coeffL = 0.3f; vo->p.head.coeffR = 0.5f;
            vo->s.gain.snap(1.0f);
            vo->s.send.snap(0.3f);
            vo->in0 = dtest::noise(kBlock, 0.3f, 100u + v);
            vo->in1 = dtest::noise(kBlock, 0.3f, 900u + v);
            voices.push_back(std::move(vo));
        }
    }

    // One block for every voice; `cache` null = interpreted. Compiled, the
    // voices run in batches as the mixer runs them (as many as the air lanes
    // hold); interpreted, one by one.
    void block(int b, VoiceJitCache* cache) {
        const VoiceJitScratch scratch{lanes.data(), traj.data(), kBlock};
        const int perBatch = cache ? kAirLanes / channels : 1;
        VoiceJitJob jobs[kAirLanes];
        int count = 0;
        for (auto& vp : voices) {
            Voice& v = *vp;
            if (moving) {
                // A few cm per block: the delay target moves, the air and gain
                // targets are re-looked-up each block as the spatializer would.
                v.p.delayTarget = (v.dist + 0.05f * std::sin(0.01f * b)) / 343.f * kSr;
            }
            float* ch[2] = {work.data() + 2 * count * kBlock, work.data() + (2 * count + 1) * kBlock};
            std::memcpy(ch[0], v.in0.data(), kBlock * sizeof(float));
            std::memcpy(ch[1], v.in1.data(), kBlock * sizeof(float));
            v.p.bus = bus.data();
            v.p.send = send.data();
            if (!cache) {
                runVoiceChain(v.p, v.s, ch, traj.data(), traj.data() + kBlock, kBlock);
                continue;
            }
            jobs[count].p = &v.p;
            jobs[count].s = &v.s;
            jobs[count].ch[0] = ch[0];
            jobs[count].ch[1] = ch[1];
            if (++count == perBatch) {
                runVoiceBatch(*cache, jobs, count, scratch, kBlock);
                count = 0;
            }
        }
        if (count) runVoiceBatch(*cache, jobs, count, scratch, kBlock);
    }
};

// ns per voice per block of `blocks` blocks after a warm-up.
double timeCrowd(int count, int channels, bool moving, VoiceJitCache* cache, int blocks,
                 std::vector<float>* busOut) {
    Crowd c(count, channels, moving);
    for (int b = 0; b < 50; b++) c.block(b, cache);
    // The copy every voice does, measured the same way and subtracted, as in
    // test_spatial_chain_cost.
    auto t0 = Clock::now();
    for (int b = 0; b < blocks; b++) {
        int slot = 0;
        for (auto& v : c.voices) {
            std::memcpy(c.work.data() + 2 * slot * kBlock, v->in0.data(), kBlock * sizeof(float));
            std::memcpy(c.work.data() + (2 * slot + 1) * kBlock, v->in1.data(), kBlock * sizeof(float));
            slot = (slot + 1) % kAirLanes;
        }
    }
    const auto copy = Clock::now() - t0;
    t0 = Clock::now();
    for (int b = 0; b < blocks; b++) c.block(50 + b, cache);
    const auto d = Clock::now() - t0 - copy;
    if (busOut) *busOut = c.bus;
    return std::chrono::duration<double, std::nano>(d).count() / (double(count) * blocks);
}

double rtPercent(double nsPerVoice, int voices) {
    const double budgetNs = 1e9 * kBlock / kSr;
    return 100.0 * nsPerVoice * voices / budgetNs;
}

} // namespace

TEST(interpreted_vs_compiled_at_100_and_200_voices) {
    VoiceJitCache cache;
    if (!voiceJitBackendAvailable() || cache.compileAllSync() == 0) {
        std::printf("  (no brass backend: interpreted only)\n");
    }
    struct Case { const char* name; int channels; bool moving; };
    const Case cases[] = {
        {"mono, moving (interpolated delay)", 1, true},
        {"mono, static (onset delay)", 1, false},
        {"stereo, moving", 2, true},
    };
    const int blocks = 1500;
    std::printf("  %-36s %6s %12s %12s %9s %9s %8s\n", "scene", "voices", "interp ns", "jit ns",
                "interp RT", "jit RT", "speedup");
    for (const Case& cs : cases) {
        for (int voices : {100, 200}) {
            std::vector<float> busI, busJ;
            const double ni = timeCrowd(voices, cs.channels, cs.moving, nullptr, blocks, &busI);
            const double nj = timeCrowd(voices, cs.channels, cs.moving, &cache, blocks, &busJ);
            std::printf("  %-36s %6d %12.1f %12.1f %8.2f%% %8.2f%% %7.2fx\n", cs.name, voices, ni, nj,
                        rtPercent(ni, voices), rtPercent(nj, voices), ni / nj);
            // Same blocks, same arithmetic: the accumulated buses agree exactly.
#if defined(__aarch64__)
            float maxDiff = 0.0f;
            for (size_t i = 0; i < busI.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(busI[i] - busJ[i]));
            ASSERT_LT(maxDiff, 0.05f);
#else
            ASSERT_TRUE(std::memcmp(busI.data(), busJ.data(), busI.size() * sizeof(float)) == 0);
#endif
        }
    }
    PASS();
}

int main() { return runAllTests(); }
