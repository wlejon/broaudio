// Compiled voice chains (spatial/voice_jit.h) against the interpreted chain.
//
// The kernels are emitted in the interpreter's operation order without FMA
// contraction, so the contract is bit-for-bit: every comparison below is
// exact. First the chain alone, shape by shape, over scenes that move every
// parameter (air targets, a moving and a settling delay, distance gain, gain
// and send ramps, pan ramps, occlusion); then two headless engines rendering
// the same scene, one forced interpreted.

#include "test_harness.h"
#include "distance_test_util.h"
#include "broaudio/spatial/voice_jit.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

using namespace broaudio;

namespace {

constexpr int kSr = 44100;

struct Line {
    PropagationDelayBuffer buf;
    explicit Line(int channels, int capacity = 16384) {
        buf.channels = channels;
        buf.capacity = capacity;
        buf.mask = capacity - 1;
        buf.data.assign(static_cast<size_t>(capacity) * channels, 0.0f);
    }
};

struct Side {
    VoiceChainState s;
    std::unique_ptr<Line> line;
    std::vector<float> bus, send;
};

float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    float m = 0.0f;
    for (size_t i = 0; i < a.size(); i++) m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

struct SceneResult {
    bool exact = false;
    float diff = 0.0f;
    int jitBlocks = 0;
    int onsetBlocks = 0;
    int movingBlocks = 0;
    float energy = 0.0f;
};

// One voice through `blocks` blocks of varying size, both ways.
SceneResult runScene(VoiceJitCache& cache, uint32_t stages, int channels, uint32_t seed) {
    static auto table = buildAirFilterTable(kSr, 20.f, 50.f);
    const int blockSizes[] = {128, 128, 64, 128, 37, 128, 256, 1, 128, 100};
    const int blocks = 160;
    int total = 0;
    for (int b = 0; b < blocks; b++) total += blockSizes[b % 10];

    Side A, B;
    for (Side* s : {&A, &B}) {
        s->line = std::make_unique<Line>(channels);
        s->bus.assign(static_cast<size_t>(total) * 2, 0.0f);
        s->send.assign(static_cast<size_t>(total) * 2, 0.0f);
        s->s.gain.init(kSr);
        s->s.distanceGain.init(kSr);
        s->s.pan.init(kSr);
        s->s.send.init(kSr);
    }
    auto inL = dtest::noise(total, 0.4f, seed);
    auto inR = dtest::noise(total, 0.4f, seed * 7u + 3u);
    std::vector<float> work0(256), work1(256), L(256), R(256);
    std::vector<float> lanes(kAirLanes * 256), traj(4 * 256);
    VoiceJitScratch scratch{lanes.data(), traj.data(), 256};

    RampControl gainRc, sendRc;
    std::atomic<float> gainTarget{0.0f}, sendTarget{0.0f};
    gainRc.set(gainTarget, 0.9f, 0.01f);     // fade in from 0 over 10 ms
    sendRc.set(sendTarget, 0.3f, 0.0f);

    SceneResult res;
    int pos = 0;
    for (int b = 0; b < blocks; b++) {
        const int n = blockSizes[b % 10];
        // Parameter events at block boundaries.
        if (b == 40) gainRc.set(gainTarget, 0.4f, 0.05f);
        if (b == 70) gainRc.set(gainTarget, 1.0f, 0.0f);   // de-zippered jump
        if (b == 55) sendRc.set(sendTarget, 0.0f, 0.02f);
        if (b == 90) sendRc.set(sendTarget, 0.6f, 0.0f);

        // Distance: approach, hold (the delay settles only by snapping, so the
        // hold after a restart below is what exercises the onset shape), recede.
        float dist;
        if (b < 50) dist = 80.f - 1.2f * b;
        else if (b < 100) dist = 20.f;
        else dist = 20.f + 0.9f * (b - 100);

        VoiceChainParams P;
        P.stages = stages;
        P.channels = channels;
        table->lookup(dist, P.airPole, P.airMix);
        P.delayTarget = dist / 343.f * kSr;
        P.delayMax = 0.3f * kSr;
        P.delaySmooth = 1.0f - std::exp(-1.0f / (0.010f * kSr));
        P.distanceGain = 1.0f / (1.0f + 0.05f * dist);
        P.pan = (stages & kStageHead) ? 0.0f : std::sin(0.07f * b);
        const float occ = 0.5f + 0.5f * std::sin(0.11f * b);
        P.head.gainL = 1.0f - 0.3f * occ;
        P.head.gainR = 0.8f - 0.2f * occ;
        P.head.coeffL = 0.2f + 0.5f * occ;
        P.head.coeffR = 0.1f + 0.6f * occ;

        // A restart of the delay line (a re-enabled delay) at block 60: the
        // next blocks snap and then hold, i.e. run the onset shape.
        if (b == 60) { A.s.delayPrimed = false; B.s.delayPrimed = false; }

        int shape = -1;
        for (Side* s : {&A, &B}) {
            s->s.gain.update(gainTarget.load(), gainRc, kSr);
            s->s.send.update(sendTarget.load(), sendRc, kSr);
            std::copy(inL.begin() + pos, inL.begin() + pos + n, work0.begin());
            std::copy(inR.begin() + pos, inR.begin() + pos + n, work1.begin());
            float* ch[2] = {work0.data(), work1.data()};
            P.delayBuf = &s->line->buf;
            P.bus = s->bus.data() + 2 * pos;
            P.send = s->send.data() + 2 * pos;
            if (s == &A) {
                runVoiceChain(P, s->s, ch, L.data(), R.data(), n);
            } else {
                shape = voiceJitShape(P, s->s);
                VoiceJitJob job;
                job.p = &P;
                job.s = &s->s;
                job.ch[0] = ch[0];
                job.ch[1] = ch[1];
                const int compiled = runVoiceBatch(cache, &job, 1, scratch, n);
                if (compiled != 1) {
                    std::printf("  block %d (shape %d) did not run compiled\n", b, shape);
                    return res;
                }
                res.jitBlocks++;
            }
        }
        const auto topo = VoiceJitTopology::fromKey(static_cast<uint32_t>(shape));
        if (topo.delay == VoiceJitTopology::Delay::Onset) res.onsetBlocks++;
        if (topo.delay == VoiceJitTopology::Delay::Interpolated) res.movingBlocks++;
        pos += n;
    }
    res.exact = sameBits(A.bus, B.bus) && sameBits(A.send, B.send);
    res.diff = std::max(maxDiff(A.bus, B.bus), maxDiff(A.send, B.send));
    for (float v : A.bus) res.energy += v * v;
    // State agrees too (what the next block would start from).
    res.exact = res.exact && A.s.delayOut == B.s.delayOut && A.s.writePos == B.s.writePos &&
                A.s.gain.value == B.s.gain.value && A.s.head.zL == B.s.head.zL &&
                std::memcmp(A.s.airZ, B.s.airZ, sizeof(A.s.airZ)) == 0 &&
                sameBits(A.line->buf.data, B.line->buf.data);
    return res;
}

} // namespace

TEST(every_shape_matches_the_interpreter_bit_for_bit) {
    if (!voiceJitBackendAvailable()) {
        std::printf("  (no brass backend: skipped)\n");
        PASS();
        return;
    }
    VoiceJitCache cache;
    ASSERT_GT(cache.compileAllSync(), 0);
    int scenes = 0;
    for (int channels = 1; channels <= 2; channels++) {
        for (uint32_t mask = 0; mask < 16; mask++) {
            uint32_t stages = 0;
            if (mask & 1) stages |= kStageAir;
            if (mask & 2) stages |= kStageDelay;
            if (mask & 4) stages |= kStageHead;
            if (mask & 8) stages |= kStageSend;
            const SceneResult r = runScene(cache, stages, channels, 1000u + mask);
            if (!r.exact) {
                std::printf("  stages 0x%x ch %d: max |diff| %g (energy %g)\n",
                            stages, channels, r.diff, r.energy);
            }
            ASSERT_TRUE(r.exact);
            ASSERT_GT(r.energy, 0.0f);
            if (stages & kStageDelay) {
                ASSERT_GT(r.onsetBlocks, 0);
                ASSERT_GT(r.movingBlocks, 0);
            }
            scenes++;
        }
    }
    std::printf("  %d scenes exact, %d kernels compiled\n", scenes, cache.publishedCount());
    PASS();
}

TEST(send_into_the_voices_own_bus_matches) {
    if (!voiceJitBackendAvailable()) { PASS(); return; }
    // The send tap may name the bus the voice already mixes into.
    VoiceJitCache cache;
    VoiceChainParams P;
    P.stages = kStageHead | kStageSend;
    P.distanceGain = 0.7f;
    P.head.gainL = 0.9f; P.head.coeffL = 0.4f;
    std::vector<float> busA(256, 0.1f), busB(256, 0.1f), L(128), R(128);
    auto x = dtest::noise(128, 0.5f);
    VoiceChainState a, b;
    a.gain.snap(1.0f); b.gain.snap(1.0f);
    a.send.snap(0.5f); b.send.snap(0.5f);
    std::vector<float> xa = x, xb = x;
    float* cha[2] = {xa.data(), xa.data()};
    float* chb[2] = {xb.data(), xb.data()};
    P.bus = P.send = busA.data();
    runVoiceChain(P, a, cha, L.data(), R.data(), 128);
    P.bus = P.send = busB.data();
    ASSERT_TRUE(cache.compileSync(static_cast<uint32_t>(voiceJitShape(P, b))));
    VoiceJitFn fn = cache.lookup(static_cast<uint32_t>(voiceJitShape(P, b)));
    ASSERT_TRUE(fn != nullptr);
    std::vector<float> traj(4 * 128);
    runVoiceChainJit(fn, P, b, chb, traj.data(), 128);
    ASSERT_TRUE(sameBits(busA, busB));
    PASS();
}

TEST(a_batch_of_mixed_voices_matches_them_one_by_one) {
    if (!voiceJitBackendAvailable()) { PASS(); return; }
    // Eight voices on one bus and one send, in order: mono and stereo with air
    // (more air channels than lanes, so one runs interpreted air), one stereo
    // source on a mono delay line (no kernel: interpreted whole), and voices
    // without air. The buses must accumulate in the same order.
    static auto table = buildAirFilterTable(kSr, 20.f, 50.f);
    VoiceJitCache cache;
    ASSERT_GT(cache.compileAllSync(), 0);
    constexpr int kV = kAirLanes, kN = 128, kBlocks = 40;
    const int chans[kV] = {2, 1, 2, 2, 1, 2, 2, 1};
    const uint32_t stg[kV] = {
        kStageAir | kStageDelay | kStageHead | kStageSend, kStageAir | kStageHead,
        kStageAir | kStageDelay | kStageSend, kStageAir | kStageDelay | kStageHead,
        kStageDelay | kStageHead | kStageSend, kStageAir | kStageHead, kStageAir,
        kStageHead | kStageSend};
    struct Voice { VoiceChainState s[2]; std::unique_ptr<Line> line[2]; std::vector<float> x[2]; };
    std::vector<Voice> v(kV);
    for (int k = 0; k < kV; k++) {
        for (int side = 0; side < 2; side++) {
            // Voice 3's line is mono under a stereo source.
            v[k].line[side] = std::make_unique<Line>(k == 3 ? 1 : chans[k]);
            v[k].s[side].gain.snap(0.8f);
            v[k].s[side].send.snap(0.25f);
        }
        v[k].x[0] = dtest::noise(kN * kBlocks, 0.3f, 50u + k);
        v[k].x[1] = dtest::noise(kN * kBlocks, 0.3f, 90u + k);
    }
    std::vector<float> bus[2], send[2];
    for (int side = 0; side < 2; side++) {
        bus[side].assign(2 * kN * kBlocks, 0.0f);
        send[side].assign(2 * kN * kBlocks, 0.0f);
    }
    std::vector<float> work(2 * kV * kN), lanes(kAirLanes * kN), traj(4 * kN);
    const VoiceJitScratch scratch{lanes.data(), traj.data(), kN};
    int compiled = 0;
    for (int b = 0; b < kBlocks; b++) {
        VoiceChainParams P[kV];
        VoiceJitJob jobs[kV];
        for (int side = 0; side < 2; side++) {
            for (int k = 0; k < kV; k++) {
                const float dist = 10.f + 12.f * k + 0.4f * b * (k % 3);
                P[k].stages = stg[k];
                P[k].channels = chans[k];
                table->lookup(dist, P[k].airPole, P[k].airMix);
                P[k].delayBuf = &v[k].line[side]->buf;
                P[k].delayTarget = dist / 343.f * kSr;
                P[k].delayMax = 0.3f * kSr;
                P[k].delaySmooth = 0.002f;
                P[k].distanceGain = 1.0f / (1.0f + 0.05f * dist);
                P[k].pan = (stg[k] & kStageHead) ? 0.0f : 0.3f;
                P[k].head.gainL = 0.9f; P[k].head.gainR = 0.7f;
                P[k].head.coeffL = 0.2f + 0.01f * k; P[k].head.coeffR = 0.4f;
                P[k].bus = bus[side].data() + 2 * kN * b;
                P[k].send = send[side].data() + 2 * kN * b;
                float* ch[2] = {work.data() + 2 * k * kN, work.data() + (2 * k + 1) * kN};
                for (int c = 0; c < 2; c++)
                    std::memcpy(ch[c], v[k].x[c].data() + kN * b, kN * sizeof(float));
                if (side == 0) {
                    runVoiceChain(P[k], v[k].s[0], ch, traj.data(), traj.data() + kN, kN);
                } else {
                    jobs[k].p = &P[k];
                    jobs[k].s = &v[k].s[1];
                    jobs[k].ch[0] = ch[0];
                    jobs[k].ch[1] = ch[1];
                }
            }
        }
        compiled += runVoiceBatch(cache, jobs, kV, scratch, kN);
    }
    std::printf("  %d of %d voice-blocks compiled, max |diff| %g\n", compiled, kV * kBlocks,
                std::max(maxDiff(bus[0], bus[1]), maxDiff(send[0], send[1])));
    // All but the mono-line stereo voice and the one whose air found no lane.
    ASSERT_EQ(compiled, (kV - 2) * kBlocks);
    ASSERT_TRUE(sameBits(bus[0], bus[1]));
    ASSERT_TRUE(sameBits(send[0], send[1]));
    PASS();
}

namespace {

// A headless engine with a busy scene: moving looped sources with air, delay
// and a ramped send, a stereo source with changing occlusion, a static
// one-shot with delay (the onset shape), and an unspatialized clip with pan
// and gain ramps.
struct Scene {
    Engine e;
    int sendBus = -1;
    int moving[3] = {-1, -1, -1};
    int stereo = -1;
    int flat = -1;

    explicit Scene(bool jit) {
        e.initHeadless();
        e.setMasterGain(0.25f);
        e.setBusJitEnabled(0, false);
        e.setVoiceJitEnabled(jit);
        if (jit) e.precompileVoiceJit();
        sendBus = e.createBus();
        e.setBusJitEnabled(sendBus, false);
        e.setBusReverbEnabled(sendBus, true);

        auto mono = dtest::noise(e.sampleRate(), 0.3f, 99u);
        const int monoClip = e.createClip(mono.data(), static_cast<int>(mono.size()), 1);
        std::vector<float> st(static_cast<size_t>(e.sampleRate()) * 2);
        auto l = dtest::sine(e.sampleRate(), 330.f, 0.3f, e.sampleRate());
        auto r = dtest::noise(e.sampleRate(), 0.2f, 5u);
        for (int i = 0; i < e.sampleRate(); i++) { st[2 * i] = l[i]; st[2 * i + 1] = r[i]; }
        const int stClip = e.createClip(st.data(), e.sampleRate(), 2);
        std::vector<float> burst(2000, 0.0f);
        for (int i = 0; i < 400; i++) burst[i] = 0.4f * std::sin(0.3f * i);
        const int burstClip = e.createClip(burst.data(), static_cast<int>(burst.size()), 1);

        for (int k = 0; k < 3; k++) {
            int pb = moving[k] = e.playClip(monoClip, 0.8f, true);
            e.setPlaybackSpatialEnabled(pb, true);
            e.setPlaybackSpatialAirAbsorption(pb, true);
            e.setPlaybackSpatialPropagationDelay(pb, true);
            e.setPlaybackSpatialPosition(pb, 10.f * k - 10.f, 0.f, -40.f - 30.f * k);
            e.setPlaybackSend(pb, sendBus, 0.2f * (k + 1), 0.05f);
        }
        stereo = e.playClip(stClip, 0.7f, true);
        e.setPlaybackSpatialEnabled(stereo, true);
        e.setPlaybackSpatialPropagationDelay(stereo, true);
        e.setPlaybackSpatialPosition(stereo, 5.f, 2.f, -15.f);

        int shot = e.playClip(burstClip, 1.0f, false);
        e.setPlaybackSpatialEnabled(shot, true);
        e.setPlaybackSpatialAirAbsorption(shot, true);
        e.setPlaybackSpatialPropagationDelay(shot, true);
        e.setPlaybackSpatialPosition(shot, -3.f, 0.f, -60.f);

        flat = e.playClip(monoClip, 0.5f, true);
    }

    void step(int frame) {
        const float t = static_cast<float>(frame) / e.sampleRate();
        for (int k = 0; k < 3; k++)
            e.setPlaybackSpatialPosition(moving[k], 30.f * std::sin(0.7f * t + k), 0.f,
                                         -40.f - 30.f * k + 25.f * std::cos(0.5f * t));
        e.setPlaybackSpatialOcclusion(stereo, 0.5f + 0.5f * std::sin(3.f * t));
        e.setPlaybackPan(flat, std::sin(2.f * t));
        if (frame == 22050) e.setPlaybackGain(flat, 0.1f, 0.2f);
        if (frame == 44100) e.setPlaybackSend(moving[1], sendBus, 0.0f, 0.3f);
    }
};

} // namespace

TEST(engine_renders_identically_with_and_without_the_jit) {
    Scene interp(false), jit(true);
    if (jit.e.isVoiceJitAvailable()) {
        ASSERT_GT(jit.e.precompileVoiceJit(), 0);
    }
    int maxJitVoices = 0;
    const int frames = 44100 * 2;
    auto a = dtest::record(interp.e, frames, 441, [&](int f) { interp.step(f); });
    auto b = dtest::record(jit.e, frames, 441, [&](int f) {
        jit.step(f);
        maxJitVoices = std::max(maxJitVoices, jit.e.voiceJitActiveVoices());
    });
    ASSERT_EQ(interp.e.voiceJitActiveVoices(), 0);
    if (jit.e.isVoiceJitAvailable()) ASSERT_GT(maxJitVoices, 4);
    std::printf("  %zu samples, max |diff| %g, up to %d voices compiled per block\n",
                a.size(), maxDiff(a, b), maxJitVoices);
    ASSERT_GT(dtest::rms(a, 0, static_cast<int>(a.size())), 1e-3);
    ASSERT_TRUE(sameBits(a, b));
    PASS();
}

TEST(kernels_arrive_without_precompiling) {
    if (!voiceJitBackendAvailable()) { PASS(); return; }
    // The audio thread only asks; the worker compiles and publishes.
    Engine e;
    e.initHeadless();
    auto x = dtest::noise(4410, 0.2f);
    const int clip = e.createClip(x.data(), static_cast<int>(x.size()), 1);
    const int pb = e.playClip(clip, 1.0f, true);
    e.setPlaybackSpatialEnabled(pb, true);
    e.setPlaybackSpatialPosition(pb, 3.f, 0.f, -7.f);
    bool compiled = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (!compiled && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
        e.renderBlock(441);   // 10 ms of audio per call, rendered as fast as it goes
        compiled = e.voiceJitActiveVoices() == 1;
        if (!compiled) std::this_thread::yield();
    }
    ASSERT_TRUE(compiled);
    e.setVoiceJitEnabled(false);
    e.renderBlock(441);
    ASSERT_EQ(e.voiceJitActiveVoices(), 0);
    PASS();
}

int main() { return runAllTests(); }
