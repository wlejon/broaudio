// Audio-thread cost of each per-voice spatial chain stage, interpreted — the
// baseline the JIT chunk is measured against. 128 mono voices spread over
// 5..300 m, 128-frame blocks at 44.1 kHz; prints ns per voice per block and
// the share of the block's real-time budget 128 voices take. Also checks the
// chain against itself: runVoiceChain equals the stages run by hand.

#include "test_harness.h"
#include "distance_test_util.h"
#include "broaudio/spatial/spatial_chain.h"

#include <chrono>
#include <cstdio>
#include <memory>

using broaudio::VoiceChainState;
using broaudio::VoiceChainParams;
using broaudio::PropagationDelayBuffer;
using broaudio::SpatialSource;
using broaudio::Listener;
using broaudio::HeadModel;
using broaudio::buildAirFilterTable;
using broaudio::computeSpatial;
using broaudio::computeHeadParams;
using broaudio::chainAir;
using broaudio::chainDelay;
using broaudio::chainGainPan;
using broaudio::chainHead;
using broaudio::chainTaps;
using broaudio::runVoiceChain;
using broaudio::kStageAir;
using broaudio::kStageDelay;
using broaudio::kStageHead;
using broaudio::kStageSend;

namespace {

constexpr int kVoices = 128;
constexpr int kBlock = 128;
constexpr int kSr = 44100;
constexpr int kBlocks = 2000;   // ~5.8 s of audio per stage

struct Voice {
    VoiceChainState s;
    VoiceChainParams p;
    PropagationDelayBuffer delay;
    SpatialSource src;
    std::vector<float> in;
};

using Clock = std::chrono::steady_clock;

double nsPerVoiceBlock(Clock::duration d) {
    return std::chrono::duration<double, std::nano>(d).count() / (double(kVoices) * kBlocks);
}

void report(const char* name, double ns) {
    const double budgetNs = 1e9 * kBlock / kSr;
    std::printf("  %-26s %8.1f ns/voice/block  %6.2f ns/sample  %5.2f%% of RT for %d voices\n",
                name, ns, ns / kBlock, 100.0 * ns * kVoices / budgetNs, kVoices);
}

} // namespace

TEST(per_stage_cost_baseline) {
    auto table = buildAirFilterTable(kSr, 20.f, 50.f);
    Listener listener;
    HeadModel head;
    std::vector<std::unique_ptr<Voice>> voices;
    auto noise = dtest::noise(kBlock, 0.3f);
    for (int v = 0; v < kVoices; v++) {
        auto vo = std::make_unique<Voice>();
        const float d = 5.f + 295.f * v / (kVoices - 1);
        vo->src.spatialEnabled = true;
        vo->src.posX = d * 0.3f;
        vo->src.posZ = -d;
        vo->delay.channels = 1;
        vo->delay.capacity = 32768;
        vo->delay.mask = 32767;
        vo->delay.data.assign(32768, 0.f);
        vo->p.channels = 1;
        vo->p.stages = kStageAir | kStageDelay | kStageHead | kStageSend;
        table->lookup(d, vo->p.airPole, vo->p.airMix);
        vo->p.delayBuf = &vo->delay;
        vo->p.delayTarget = d / 343.f * kSr;
        vo->p.delayMax = 0.5f * kSr;
        vo->p.delaySmooth = 1.f - std::exp(-1.f / (0.01f * kSr));
        vo->s.gain.snap(1.0f);
        vo->s.send.snap(0.3f);
        vo->in = noise;
        voices.push_back(std::move(vo));
    }
    std::vector<float> L(kBlock), R(kBlock), bus(2 * kBlock), send(2 * kBlock);

    // Spatializer: the per-block parameter work (distance, head model, air lookup).
    auto t0 = Clock::now();
    for (int b = 0; b < kBlocks; b++) {
        for (auto& v : voices) {
            auto sr = computeSpatial(listener, v->src);
            v->p.distanceGain = sr.gain;
            v->p.head = computeHeadParams(sr, head, kSr, 0.2f);
            const float m = bromath::vlen(v->src.position() - listener.position());
            table->lookup(m, v->p.airPole, v->p.airMix);
            v->p.delayTarget = m / 343.f * kSr;
        }
    }
    report("spatializer (params)", nsPerVoiceBlock(Clock::now() - t0));

    std::vector<float> work(kBlock);
    float* ch[1] = {work.data()};
    auto timeStage = [&](const char* name, auto&& stage) {
        auto t = Clock::now();
        for (int b = 0; b < kBlocks; b++) {
            for (auto& v : voices) {
                std::copy(v->in.begin(), v->in.end(), work.begin());
                stage(*v);
            }
        }
        const auto d = Clock::now() - t;
        return d;
    };
    // Baseline: the copy every timed loop does, subtracted from each stage.
    const auto copyCost = timeStage("copy", [](Voice&) {});
    auto run = [&](const char* name, auto&& stage) {
        auto d = timeStage(name, stage);
        report(name, nsPerVoiceBlock(d - copyCost));
    };
    run("air (7 sections, mono)", [&](Voice& v) { chainAir(v.s, v.p.airPole, v.p.airMix, ch, 1, kBlock); });
    run("delay (Hermite, mono)", [&](Voice& v) {
        chainDelay(v.s, v.delay, v.p.delayTarget, v.p.delayMax, v.p.delaySmooth, ch, 1, kBlock); });
    run("gain/pan (mono->stereo)", [&](Voice& v) {
        chainGainPan(v.s, v.p.distanceGain, 0.f, ch, 1, L.data(), R.data(), kBlock); });
    run("head + occlusion", [&](Voice& v) { chainHead(v.s, v.p.head, L.data(), R.data(), kBlock); });
    run("taps (bus + send)", [&](Voice& v) {
        chainTaps(v.s, L.data(), R.data(), bus.data(), send.data(), kBlock); });
    run("whole chain", [&](Voice& v) {
        v.p.bus = bus.data();
        v.p.send = send.data();
        runVoiceChain(v.p, v.s, ch, L.data(), R.data(), kBlock);
    });
    PASS();
}

TEST(run_voice_chain_equals_its_stages) {
    auto table = buildAirFilterTable(kSr, 20.f, 50.f);
    auto x = dtest::noise(1024, 0.3f);
    PropagationDelayBuffer d1, d2;
    for (auto* d : {&d1, &d2}) {
        d->channels = 1; d->capacity = 8192; d->mask = 8191; d->data.assign(8192, 0.f);
    }
    VoiceChainParams p;
    p.stages = kStageAir | kStageDelay | kStageHead | kStageSend;
    table->lookup(120.f, p.airPole, p.airMix);
    p.delayTarget = 1000.3f;
    p.delayMax = 4000.f;
    p.delaySmooth = 0.01f;
    p.distanceGain = 0.7f;
    p.head.coeffL = 0.3f; p.head.coeffR = 0.5f; p.head.gainL = 0.9f; p.head.gainR = 0.6f;
    std::vector<float> busA(2048, 0.f), sendA(2048, 0.f), busB(2048, 0.f), sendB(2048, 0.f);
    std::vector<float> L(1024), R(1024);
    VoiceChainState a, b;
    a.gain.snap(0.8f); a.send.snap(0.25f);
    b.gain.snap(0.8f); b.send.snap(0.25f);
    std::vector<float> xa = x, xb = x;
    float* cha[1] = {xa.data()};
    float* chb[1] = {xb.data()};
    p.bus = busA.data(); p.send = sendA.data(); p.delayBuf = &d1;
    runVoiceChain(p, a, cha, L.data(), R.data(), 1024);
    chainAir(b, p.airPole, p.airMix, chb, 1, 1024);
    chainDelay(b, d2, p.delayTarget, p.delayMax, p.delaySmooth, chb, 1, 1024);
    chainGainPan(b, p.distanceGain, 0.f, chb, 1, L.data(), R.data(), 1024);
    chainHead(b, p.head, L.data(), R.data(), 1024);
    chainTaps(b, L.data(), R.data(), busB.data(), sendB.data(), 1024);
    for (int i = 0; i < 2048; i++) {
        ASSERT_NEAR(busA[i], busB[i], 1e-7f);
        ASSERT_NEAR(sendA[i], sendB[i], 1e-7f);
    }
    // The delay of 1000.3 frames holds the output silent until the 4-point
    // interpolator's window first reaches input frame 0 (output frame 999).
    ASSERT_NEAR(busA[2 * 998], 0.0f, 1e-9f);
    ASSERT_GT(std::fabs(busA[2 * 1010]) + std::fabs(busA[2 * 1011]), 0.0f);
    PASS();
}

int main() { return runAllTests(); }
