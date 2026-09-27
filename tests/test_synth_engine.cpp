// Synthesis voices in the engine: the source stage of the per-voice chain
// produces exactly the offline render (compiled and interpreted), a synth
// playback behaves like a clip playback (plays, schedules, stops, releases,
// spatializes, finishes by itself), and two engines, one forced interpreted,
// render a scene of synth voices identically.

#include "test_harness.h"
#include "distance_test_util.h"
#include "synth_test_graphs.h"
#include "broaudio/spatial/voice_jit.h"
#include "broaudio/synth/synth_graph.h"

#include <cstring>

using namespace broaudio;

namespace {

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
#if defined(__aarch64__)
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i] - b[i]) > 0.02f) return false;
    }
    return true;
#else
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
#endif
}

const char* kGraphs[] = {synthtest::kGunshot, synthtest::kFmBell, synthtest::kMetalHit, synthtest::kKitchenSink};

} // namespace

TEST(source_stage_equals_the_offline_render)
{
    VoiceJitCache cache;
    const bool jit = voiceJitBackendAvailable() && cache.compileAllSync() > 0;
    constexpr int kSr = 44100;
    const int sizes[] = {128, 64, 441, 1, 300};
    for (const char* desc : kGraphs) {
        auto g = SynthGraph::fromJson(desc);
        if (jit) ASSERT_TRUE(g->precompile());
        SynthTrigger t;
        t.seed = 42;
        SynthRenderOptions o;
        o.sampleRate = kSr;
        o.trigger = t;
        o.maxSeconds = 3.0;
        const std::vector<float> offline = renderSynth(g, o);

        // Two voices through the chain: batch (compiled) and runVoiceChain
        // (interpreted). No air or delay, so ch[0] keeps the source block.
        SynthVoice va(g, kSr, t), vb(g, kSr, t);
        VoiceChainState sa, sb;
        for (VoiceChainState* s : {&sa, &sb}) {
            s->gain.init(kSr); s->distanceGain.init(kSr); s->pan.init(kSr); s->send.init(kSr);
            s->gain.snap(1.0f);
        }
        std::vector<float> outA, outB, bus(2 * 512), work(2 * kAirLanes * 512), lanes(kAirLanes * 512),
            traj(4 * 512);
        int compiledBlocks = 0, blocks = 0;
        for (int b = 0; static_cast<int>(outA.size()) < o.maxSeconds * kSr; b++) {
            const int n = sizes[b % 5];
            VoiceChainParams P;
            P.stages = kStageSource;
            P.bus = bus.data();
            P.sourceFrom = 0;
            P.sourceTo = n;
            P.source = &va;
            VoiceJitJob job;
            job.p = &P;
            job.s = &sa;
            job.ch[0] = work.data();
            job.ch[1] = work.data() + 512;
            compiledBlocks += runVoiceBatch(cache, &job, 1, VoiceJitScratch{lanes.data(), traj.data(), 512}, n);
            blocks++;
            outA.insert(outA.end(), work.begin(), work.begin() + n);
            P.source = &vb;
            float* ch[2] = {work.data(), work.data() + 512};
            runVoiceChain(P, sb, ch, traj.data(), traj.data() + 512, n);
            outB.insert(outB.end(), work.begin(), work.begin() + n);
            if (va.finished() && vb.finished()) break;
        }
        ASSERT_TRUE(outA.size() >= offline.size());
        std::vector<float> headA(outA.begin(), outA.begin() + offline.size());
        std::vector<float> headB(outB.begin(), outB.begin() + offline.size());
        ASSERT_TRUE(sameBits(headA, offline));
        ASSERT_TRUE(sameBits(headB, offline));
        for (size_t i = offline.size(); i < outA.size(); i++) ASSERT_TRUE(outA[i] == 0.0f && outB[i] == 0.0f);
        if (jit) ASSERT_EQ(compiledBlocks, blocks);
    }
    PASS();
}

TEST(a_synth_playback_behaves_like_a_clip_playback)
{
    Engine e;
    ASSERT_TRUE(e.initHeadless());
    e.setMasterGain(0.25f);
    const int sr = e.sampleRate();
    auto bell = SynthGraph::fromJson(synthtest::kFmBell);
    ASSERT_EQ(e.playSynth(nullptr, {}), -1);

    // Plays and finishes by itself.
    const int pb = e.playSynth(bell, {});
    ASSERT_TRUE(pb > 0);
    auto rec = dtest::record(e, sr / 5, 512);
    ASSERT_GT(dtest::rms(rec, 0, static_cast<int>(rec.size())), 1e-3);
    ASSERT_TRUE(e.getPlaybackState(pb) == Engine::PlaybackState::Playing);
    int frames = 0;
    while (e.getPlaybackState(pb) == Engine::PlaybackState::Playing && frames < sr * 10) {
        e.renderBlock(512);
        frames += 512;
    }
    ASSERT_TRUE(frames < sr * 10);

    // Scheduled start, scheduled stop.
    Engine::SynthPlayOptions later;
    later.when = e.currentTime() + 0.05;
    const int sched = e.playSynth(bell, later);
    ASSERT_TRUE(e.getPlaybackState(sched) == Engine::PlaybackState::Scheduled);
    e.renderBlock(sr / 10);
    ASSERT_TRUE(e.getPlaybackState(sched) == Engine::PlaybackState::Playing);
    e.stopPlaybackAt(sched, e.currentTime() + 0.02);
    e.renderBlock(sr / 20);
    ASSERT_TRUE(e.getPlaybackState(sched) != Engine::PlaybackState::Playing);

    // A sustained envelope holds until releaseSynth.
    auto pad = SynthGraph::fromJson(R"({"nodes": {
        "o": {"type": "osc", "wave": "saw", "freq": 220, "gain": "e"},
        "e": {"type": "env", "attack": 0.01, "decay": 0.05, "sustain": 0.5, "release": 0.1}}, "output": "o"})");
    const int held = e.playSynth(pad, {});
    for (int k = 0; k < 20; k++) e.renderBlock(sr / 10);
    ASSERT_TRUE(e.getPlaybackState(held) == Engine::PlaybackState::Playing);
    e.releaseSynth(held);
    for (int k = 0; k < 5; k++) e.renderBlock(sr / 10);
    ASSERT_TRUE(e.getPlaybackState(held) != Engine::PlaybackState::Playing);

    // Positional: a source to the left is louder on the left.
    const int left = e.playSynth(pad, {});
    e.setPlaybackSpatialEnabled(left, true);
    e.setPlaybackSpatialPosition(left, -4.f, 0.f, 0.f);
    auto st = dtest::record(e, sr / 2, 512);
    double l = 0, r = 0;
    for (size_t i = sr / 10; i + 1 < st.size(); i += 2) { l += st[i] * st[i]; r += st[i + 1] * st[i + 1]; }
    ASSERT_GT(l, 2.0 * r);
    e.stopPlayback(left);
    e.shutdown();
    PASS();
}

TEST(a_looping_voice_is_the_offline_loop_in_the_engine)
{
    // The source stage (compiled batch and interpreted chain) plays the
    // offline loop period after period.
    VoiceJitCache cache;
    const bool jit = voiceJitBackendAvailable() && cache.compileAllSync() > 0;
    constexpr int kSr = 44100;
    auto g = SynthGraph::fromJson(R"({"nodes": {
        "o":  {"type": "osc", "wave": "saw", "freq": 97, "gain": "e"},
        "t":  {"type": "impulses", "shape": "hann", "rate": 11, "length": 0.02, "jitter": 0.3},
        "n":  {"type": "noise", "color": "pink"},
        "tn": {"type": "mul", "a": "n", "b": "t", "gain": 0.5},
        "m":  {"type": "mix", "inputs": ["o", "tn"]},
        "e":  {"type": "env", "attack": 0.02, "decay": 0.1, "sustain": 0.7, "release": 0.2},
        "lp": {"type": "filter", "input": "m", "cutoff": 1500, "q": 1.5}}, "output": "lp"})");
    SynthLoopOptions loop;
    loop.length = 0.3;
    loop.crossfade = 0.04;
    SynthRenderOptions o;
    o.sampleRate = kSr;
    o.trigger.seed = 8;
    o.loop = loop;
    const std::vector<float> offline = renderSynth(g, o);
    ASSERT_EQ(static_cast<int>(offline.size()), static_cast<int>(0.3 * kSr + 0.5));

    SynthVoice va(g, kSr, o.trigger, loop), vb(g, kSr, o.trigger, loop);
    VoiceChainState sa, sb;
    for (VoiceChainState* s : {&sa, &sb}) {
        s->gain.init(kSr); s->distanceGain.init(kSr); s->pan.init(kSr); s->send.init(kSr);
        s->gain.snap(1.0f);
    }
    std::vector<float> outA, outB, bus(2 * 512), work(2 * kAirLanes * 512), lanes(kAirLanes * 512), traj(4 * 512);
    const int sizes[] = {128, 64, 441, 1, 300};
    for (int b = 0; outA.size() < 4 * offline.size(); b++) {
        const int n = sizes[b % 5];
        VoiceChainParams P;
        P.stages = kStageSource;
        P.bus = bus.data();
        P.sourceFrom = 0;
        P.sourceTo = n;
        P.source = &va;
        VoiceJitJob job;
        job.p = &P;
        job.s = &sa;
        job.ch[0] = work.data();
        job.ch[1] = work.data() + 512;
        if (jit) runVoiceBatch(cache, &job, 1, VoiceJitScratch{lanes.data(), traj.data(), 512}, n);
        else runVoiceChain(P, sa, job.ch, traj.data(), traj.data() + 512, n);
        outA.insert(outA.end(), work.begin(), work.begin() + n);
        P.source = &vb;
        float* ch[2] = {work.data(), work.data() + 512};
        runVoiceChain(P, sb, ch, traj.data(), traj.data() + 512, n);
        outB.insert(outB.end(), work.begin(), work.begin() + n);
    }
    for (size_t i = 0; i < outA.size(); i++) {
        ASSERT_TRUE(std::memcmp(&outA[i], &offline[i % offline.size()], sizeof(float)) == 0);
        ASSERT_TRUE(std::memcmp(&outB[i], &offline[i % offline.size()], sizeof(float)) == 0);
    }

    // Through the engine: periodic until releaseSynth, then it ends.
    Engine e;
    ASSERT_TRUE(e.initHeadless());
    e.setMasterGain(0.25f);
    const int sr = e.sampleRate();
    Engine::SynthPlayOptions po;
    po.trigger.seed = 8;
    po.loop = loop;
    const int pb = e.playSynth(g, po);
    const int period = static_cast<int>(std::llround(0.3 * sr));
    auto rec = dtest::record(e, period * 5, 512);
    ASSERT_TRUE(e.getPlaybackState(pb) == Engine::PlaybackState::Playing);
    ASSERT_GT(dtest::rms(rec, 0, static_cast<int>(rec.size())), 1e-3);
    // Periods 3 and 4 (gains long settled) are the same samples.
    std::vector<float> p3(rec.begin() + 2 * 3 * period, rec.begin() + 2 * 4 * period);
    std::vector<float> p4(rec.begin() + 2 * 4 * period, rec.begin() + 2 * 5 * period);
    ASSERT_TRUE(sameBits(p3, p4));
    e.releaseSynth(pb);
    int frames = 0;
    while (e.getPlaybackState(pb) == Engine::PlaybackState::Playing && frames < sr * 3) {
        e.renderBlock(512);
        frames += 512;
    }
    ASSERT_TRUE(frames > sr / 10 && frames < sr);   // the 0.2 s release, not the loop
    e.shutdown();
    PASS();
}

namespace {

// Synth voices of every kind: spatial ones with air, delay and a send,
// scheduled ones, a released pad.
struct SynthScene {
    Engine e;
    int send = -1;
    std::vector<std::shared_ptr<const SynthGraph>> graphs;
    int pad = -1;

    explicit SynthScene(bool jit) {
        e.initHeadless();
        e.setMasterGain(0.2f);
        e.setBusJitEnabled(0, false);
        e.setVoiceJitEnabled(jit);
        if (jit) e.precompileVoiceJit();
        send = e.createBus();
        e.setBusJitEnabled(send, false);
        e.setBusReverbEnabled(send, true);
        for (const char* d : kGraphs) {
            graphs.push_back(SynthGraph::fromJson(d));
            if (jit) graphs.back()->precompile();
        }
    }

    int next = 0;

    void step(int frame) {
        const int sr = e.sampleRate();
        while (frame >= next * (sr / 8) && next < 16) trigger(next++);
    }

    void trigger(int k) {
        Engine::SynthPlayOptions o;
        o.trigger.seed = static_cast<uint32_t>(k * 31 + 7);
        o.gain = 0.5f;
        o.when = e.currentTime() + 0.003 * (k % 3);
        const int pb = e.playSynth(graphs[k % graphs.size()], o);
        if (k % 2 == 0) {
            e.setPlaybackSpatialEnabled(pb, true);
            e.setPlaybackSpatialAirAbsorption(pb, true);
            e.setPlaybackSpatialPropagationDelay(pb, true);
            e.setPlaybackSpatialPosition(pb, -20.f + 5.f * k, 0.f, -30.f - 10.f * (k % 4));
            e.setPlaybackSend(pb, send, 0.3f);
        } else {
            e.setPlaybackPan(pb, (k % 5) * 0.4f - 0.8f);
        }
        if (k == 3) pad = pb;
        if (k == 9) e.releaseSynth(pad);
    }
};

} // namespace

TEST(engine_renders_synth_voices_identically_with_and_without_the_jit)
{
    SynthScene interp(false), jit(true);
    const int frames = interp.e.sampleRate() * 3;
    int maxJit = 0;
    auto a = dtest::record(interp.e, frames, 441, [&](int f) { interp.step(f); });
    auto b = dtest::record(jit.e, frames, 441, [&](int f) {
        jit.step(f);
        maxJit = std::max(maxJit, jit.e.voiceJitActiveVoices());
    });
    ASSERT_EQ(interp.e.voiceJitActiveVoices(), 0);
    if (jit.e.isVoiceJitAvailable()) ASSERT_GT(maxJit, 2);
    std::printf("  %zu samples, up to %d synth voices compiled per block\n", a.size(), maxJit);
    ASSERT_GT(dtest::rms(a, 0, static_cast<int>(a.size())), 1e-3);
    ASSERT_TRUE(sameBits(a, b));
    PASS();
}

int main() { return runAllTests(); }
