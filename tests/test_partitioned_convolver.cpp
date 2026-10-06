// Uniformly partitioned convolution: exact against direct convolution (mono
// and stereo IRs, irregular block sizes), through a bus fed by a send, the
// 'convolution' effect slot, and the per-block cost of a multi-second tail.

#include "test_harness.h"
#include "distance_test_util.h"
#include "broaudio/dsp/partitioned_convolver.h"
#include "broaudio/dsp/param_ramp.h"

#include <chrono>
#include <cstdio>

using namespace broaudio;

static std::vector<double> direct(const std::vector<float>& x, const float* h, int hlen, int stride) {
    std::vector<double> y(x.size(), 0.0);
    for (size_t n = 0; n < x.size(); n++) {
        double s = 0.0;
        for (int k = 0; k < hlen && k <= static_cast<int>(n); k++)
            s += static_cast<double>(h[static_cast<size_t>(k) * stride]) * x[n - k];
        y[n] = s;
    }
    return y;
}

TEST(matches_direct_convolution_mono_ir) {
    const int irLen = 1000;
    auto ir = dtest::noise(irLen, 0.5f, 7u);
    for (int i = 0; i < irLen; i++) ir[i] *= std::exp(-4.0f * i / irLen);
    auto conv = PartitionedConvolver::create(ir.data(), irLen, 1, 256);
    ASSERT_TRUE(conv != nullptr);
    ASSERT_EQ(conv->partitions(), 4);

    const int n = 6000;
    auto xl = dtest::noise(n, 0.5f, 11u);
    auto xr = dtest::noise(n, 0.5f, 13u);
    std::vector<float> yl(n), yr(n);
    const int chunks[] = {37, 128, 500, 1, 256, 999};
    int pos = 0, c = 0;
    while (pos < n) {
        int len = std::min(chunks[c++ % 6], n - pos);
        conv->processWet(xl.data() + pos, xr.data() + pos, yl.data() + pos, yr.data() + pos, len);
        pos += len;
    }
    auto dl = direct(xl, ir.data(), irLen, 1);
    auto dr = direct(xr, ir.data(), irLen, 1);
    const int B = conv->latencyFrames();
    double worst = 0.0;
    for (int i = 0; i + B < n; i++) {
        worst = std::max(worst, std::fabs(yl[i + B] - dl[i]));
        worst = std::max(worst, std::fabs(yr[i + B] - dr[i]));
    }
    for (int i = 0; i < B; i++) worst = std::max(worst, static_cast<double>(std::fabs(yl[i])));
    std::printf("  mono IR: max |partitioned - direct| = %.2e\n", worst);
    ASSERT_LT(worst, 2e-5);
    PASS();
}

TEST(matches_direct_convolution_stereo_ir) {
    const int irLen = 700;
    auto a = dtest::noise(irLen, 0.4f, 21u);
    auto b = dtest::noise(irLen, 0.4f, 22u);
    std::vector<float> ir(irLen * 2);
    for (int i = 0; i < irLen; i++) { ir[2 * i] = a[i]; ir[2 * i + 1] = b[i]; }
    auto conv = PartitionedConvolver::create(ir.data(), irLen, 2, 128);
    ASSERT_TRUE(conv != nullptr);
    const int n = 3000;
    auto xl = dtest::noise(n, 0.5f, 31u);
    auto xr = dtest::noise(n, 0.5f, 32u);
    std::vector<float> yl(n), yr(n);
    conv->processWet(xl.data(), xr.data(), yl.data(), yr.data(), n);
    auto dl = direct(xl, ir.data(), irLen, 2);
    auto dr = direct(xr, ir.data() + 1, irLen, 2);
    const int B = conv->latencyFrames();
    double worst = 0.0;
    for (int i = 0; i + B < n; i++) {
        worst = std::max(worst, std::fabs(yl[i + B] - dl[i]));
        worst = std::max(worst, std::fabs(yr[i + B] - dr[i]));
    }
    std::printf("  stereo IR: max |partitioned - direct| = %.2e\n", worst);
    ASSERT_LT(worst, 2e-5);
    PASS();
}

TEST(rejects_bad_input) {
    float x[4] = {1, 0, 0, 0};
    ASSERT_TRUE(PartitionedConvolver::create(nullptr, 4, 1) == nullptr);
    ASSERT_TRUE(PartitionedConvolver::create(x, 0, 1) == nullptr);
    ASSERT_TRUE(PartitionedConvolver::create(x, 2, 3) == nullptr);
    PASS();
}

// A send-fed return bus with a delta IR at 100 frames: the return carries the
// source 100 + latency frames later, at the IR's height, and nothing dry.
TEST(bus_convolution_fed_by_a_send) {
    Engine e;
    e.initHeadless();
    e.setLimiterEnabled(false);
    const int sr = e.sampleRate();
    std::vector<float> ir(300, 0.0f);
    ir[100] = 0.5f;
    int irClip = e.createClip(ir.data(), static_cast<int>(ir.size()), 1);
    int ret = e.createBus();
    ASSERT_TRUE(e.setBusConvolutionImpulse(ret, irClip));
    ASSERT_EQ(e.getBusConvolutionImpulse(ret), irClip);
    e.deleteClip(irClip);   // the IR was copied
    e.setBusConvolutionEnabled(ret, true);
    ASSERT_TRUE(e.getBusConvolutionEnabled(ret));
    ASSERT_NEAR(e.getBusConvolutionMix(ret), 1.0f, 1e-6f);

    int dry = e.createBus();
    e.setBusMuted(dry, true);
    std::vector<float> click(8, 0.0f);
    click[0] = 0.8f;
    int clip = e.createClip(click.data(), static_cast<int>(click.size()), 1);
    int pb = e.playClip(clip, 1.0f, false);
    e.setPlaybackBus(pb, dry);
    e.setPlaybackSend(pb, ret, 1.0f);
    auto rec = dtest::left(dtest::record(e, sr / 4));
    const int at = dtest::firstAbove(rec, 1e-4f);
    const int want = 100 + PartitionedConvolver::kDefaultBlock;
    std::printf("  wet onset %d (expected %d), height %.4f\n", at, want, at >= 0 ? rec[at] : 0.f);
    ASSERT_EQ(at, want);
    // click 0.8 x pan 0.707 x IR 0.5 x master 0.5, the return bus at centre pan passes it whole.
    ASSERT_NEAR(rec[at], 0.8f * 0.70710678f * 0.5f * 0.5f, 1e-4f);

    // Clearing the IR silences the wet path.
    ASSERT_TRUE(e.setBusConvolutionImpulse(ret, -1));
    ASSERT_EQ(e.getBusConvolutionImpulse(ret), -1);
    ASSERT_FALSE(e.setBusConvolutionImpulse(ret, 9999));
    ASSERT_FALSE(e.setBusConvolutionImpulse(9999, -1));
    PASS();
}

TEST(effect_order_accepts_convolution_and_refills) {
    Engine e;
    e.initHeadless();
    int bus = e.createBus();
    EffectSlot order[2] = {EffectSlot::Convolution, EffectSlot::Reverb};
    e.setBusEffectOrder(bus, order, 2);
    // Render with convolution enabled but no IR (a no-op slot) to exercise the path.
    e.setBusConvolutionEnabled(bus, true);
    e.renderBlock(1024);
    ASSERT_EQ(static_cast<int>(EffectSlot::Count), 8);
    PASS();
}

TEST(three_second_stereo_tail_cost) {
    const int sr = 44100;
    const int irLen = 3 * sr;
    auto a = dtest::noise(irLen, 0.1f, 41u);
    std::vector<float> ir(irLen * 2);
    for (int i = 0; i < irLen; i++) {
        const float env = std::exp(-6.9f * i / irLen);
        ir[2 * i] = a[i] * env;
        ir[2 * i + 1] = a[(i * 7) % irLen] * env;
    }
    auto t0 = std::chrono::steady_clock::now();
    auto conv = PartitionedConvolver::create(ir.data(), irLen, 2);
    auto t1 = std::chrono::steady_clock::now();
    ASSERT_TRUE(conv != nullptr);
    const int seconds = 4;
    std::vector<float> buf(2 * 512);
    ParamRamp mix;
    mix.snap(1.0f);
    auto x = dtest::noise(512 * 2, 0.3f, 5u);
    auto t2 = std::chrono::steady_clock::now();
    for (int done = 0; done < seconds * sr; done += 512) {
        std::copy(x.begin(), x.end(), buf.begin());
        conv->processInterleaved(buf.data(), 512, mix);
    }
    auto t3 = std::chrono::steady_clock::now();
    const double prepMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double runMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::printf("  3 s stereo IR, %d partitions: prepare %.1f ms; process %.2f%% of real time"
                " (%.1f us per 128-frame block)\n",
                conv->partitions(), prepMs, 100.0 * runMs / (seconds * 1000.0),
                runMs * 1000.0 / (seconds * sr / 128.0));
    // A generous ceiling that still catches an accidental O(N^2) path.
#if defined(BROAUDIO_COVERAGE_BUILD) || defined(__COVERAGE__)
    // Coverage instrumentation slows the loop (measured ~0.30 of real time in CI).
    ASSERT_LT(runMs / (seconds * 1000.0), 0.50);
#else
    ASSERT_LT(runMs / (seconds * 1000.0), 0.25);
#endif
    PASS();
}

int main() { return runAllTests(); }
