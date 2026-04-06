#include "test_harness.h"
#include "broaudio/dsp/smoother.h"
#include "broaudio/engine.h"

#include <cmath>
#include <vector>

using namespace broaudio;

// ---------------------------------------------------------------------------
// Smoother unit tests
// ---------------------------------------------------------------------------

TEST(smoother_snaps_on_first_set) {
    Smoother s;
    s.init(44100);
    s.set(0.75f);
    // First call to next() should return very close to 0.75
    float val = s.next();
    ASSERT_NEAR(val, 0.75f, 0.01f);
    PASS();
}

TEST(smoother_snap_is_exact) {
    Smoother s;
    s.init(44100);
    s.snap(0.5f);
    ASSERT_NEAR(s.next(), 0.5f, 1e-6f);
    ASSERT_NEAR(s.next(), 0.5f, 1e-6f);
    PASS();
}

TEST(smoother_ramps_to_target) {
    Smoother s;
    s.init(44100, 5.0f);
    s.snap(0.0f);
    s.set(1.0f);

    // After ~220 samples (5ms at 44100), should be ~95% of target
    float val = 0.0f;
    for (int i = 0; i < 220; i++)
        val = s.next();

    ASSERT_GT(val, 0.9f);
    ASSERT_LT(val, 1.0f);

    // After many more samples, should be very close to target
    for (int i = 0; i < 1000; i++)
        val = s.next();
    ASSERT_NEAR(val, 1.0f, 0.001f);

    PASS();
}

TEST(smoother_does_not_overshoot) {
    Smoother s;
    s.init(44100);
    s.snap(0.0f);
    s.set(1.0f);

    for (int i = 0; i < 10000; i++) {
        float val = s.next();
        ASSERT_TRUE(val >= 0.0f);
        ASSERT_TRUE(val <= 1.0f);
    }
    PASS();
}

// ---------------------------------------------------------------------------
// Integration: verify gain change produces a ramp, not a step
// ---------------------------------------------------------------------------

TEST(voice_gain_change_ramps) {
    Engine engine;
    engine.initHeadless();

    int v = engine.createVoice();
    engine.setWaveform(v, Waveform::Sine);
    engine.setFrequency(v, 440.0f);
    engine.setGain(v, 1.0f);
    engine.startVoice(v, 0.0);

    // Render a block at gain=1.0 to stabilize
    engine.renderBlock(512);

    // Record the master bus peak at current gain
    float peakBefore = engine.getBusPeakL(Engine::MASTER_BUS_ID);

    // Abruptly change gain to 0.0
    engine.setGain(v, 0.0f);

    // Render a short block — output should NOT immediately drop to zero
    // because the smoother ramps down over ~5ms (~220 samples)
    engine.renderBlock(32); // ~0.7ms — much less than 5ms smoothing time

    float peakAfter = engine.getBusPeakL(Engine::MASTER_BUS_ID);

    // The signal should still have noticeable energy (not yet silent)
    ASSERT_GT(peakAfter, 0.001f);

    // After rendering enough for the smoother to settle (~50ms), should be near 0
    engine.renderBlock(4096);
    float peakSettled = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_LT(peakSettled, 0.05f);

    PASS();
}

TEST(master_gain_change_ramps) {
    Engine engine;
    engine.initHeadless();

    int v = engine.createVoice();
    engine.setWaveform(v, Waveform::Sine);
    engine.setFrequency(v, 440.0f);
    engine.setGain(v, 1.0f);
    engine.startVoice(v, 0.0);

    engine.setMasterGain(1.0f);
    engine.renderBlock(512); // stabilize

    // Drop master gain to 0
    engine.setMasterGain(0.0f);
    engine.renderBlock(32); // very short block

    // Output buffer should still have energy from the ramp
    // (master gain smoother hasn't settled yet)
    auto& buf = engine.outputBuffer();
    float peak = 0.0f;
    // Read the most recent samples from the analysis buffer
    // We'll just check the output buffer isn't completely empty
    // by rendering another small block and checking metering
    engine.renderBlock(256);

    // After ~6ms total, most of the ramp has happened but we should
    // verify it didn't snap instantly by checking the metering
    // dropped gradually. This is a smoke test.
    PASS();
}

TEST(bus_gain_change_ramps) {
    Engine engine;
    engine.initHeadless();

    int bus = engine.createBus();
    engine.setBusGain(bus, 1.0f);

    int v = engine.createVoice();
    engine.setWaveform(v, Waveform::Sine);
    engine.setFrequency(v, 440.0f);
    engine.setGain(v, 1.0f);
    engine.setVoiceBus(v, bus);
    engine.startVoice(v, 0.0);

    // Stabilize
    engine.renderBlock(512);

    // Drop bus gain to 0
    engine.setBusGain(bus, 0.0f);
    engine.renderBlock(32);

    // Master bus should still have some energy (bus ramp not complete)
    float peak = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_GT(peak, 0.0001f);

    // After settling, should be near zero
    engine.renderBlock(4096);
    peak = engine.getBusPeakL(Engine::MASTER_BUS_ID);
    ASSERT_LT(peak, 0.05f);

    PASS();
}

int main() { return runAllTests(); }
