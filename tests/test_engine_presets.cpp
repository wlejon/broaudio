#include "test_harness.h"
#include "broaudio/engine.h"
#include "broaudio/io/serialization.h"

#include <cmath>

using namespace broaudio;

// ---------------------------------------------------------------------------
// applyVoicePreset
// ---------------------------------------------------------------------------

TEST(apply_voice_preset_full) {
    Engine e; e.initHeadless();
    int v = e.createVoice();

    VoicePreset p;
    p.waveform = Waveform::Sawtooth;
    p.frequency = 220.0f;
    p.gain = 0.5f;
    p.pan = 0.3f;
    p.pitchBend = 1.0f;
    p.attackTime = 0.02f;
    p.decayTime = 0.05f;
    p.sustainLevel = 0.7f;
    p.releaseTime = 0.1f;
    p.filterEnabled = true;
    p.filterType = BiquadFilter::Type::Lowpass;
    p.filterFreq = 800.0f;
    p.filterQ = 2.0f;
    p.unisonCount = 3;
    p.unisonDetune = 0.2f;
    p.unisonStereoWidth = 0.5f;

    e.applyVoicePreset(v, p);
    e.startVoice(v, 0.0);
    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);
    PASS();
}

TEST(apply_voice_preset_invalid_id_safe) {
    Engine e; e.initHeadless();
    VoicePreset p;
    e.applyVoicePreset(9999, p);
    PASS();
}

// ---------------------------------------------------------------------------
// applyBusPreset
// ---------------------------------------------------------------------------

TEST(apply_bus_preset_with_all_effects_enabled) {
    Engine e; e.initHeadless();

    BusPreset p;
    p.gain = 0.6f;
    p.pan = -0.2f;

    // Custom order
    p.effectOrder[0] = EffectSlot::Filter;
    p.effectOrder[1] = EffectSlot::Compressor;
    p.effectOrder[2] = EffectSlot::Equalizer;
    p.effectOrder[3] = EffectSlot::Distortion;
    p.effectOrder[4] = EffectSlot::Chorus;
    p.effectOrder[5] = EffectSlot::Delay;
    p.effectOrder[6] = EffectSlot::Reverb;

    p.filters[0].enabled = true;
    p.filters[0].type = BiquadFilter::Type::Highpass;
    p.filters[0].frequency = 200.0f;
    p.filters[0].Q = 0.7f;

    p.delay.enabled = true;
    p.delay.time = 0.15f;
    p.delay.feedback = 0.4f;
    p.delay.mix = 0.3f;

    p.compressor.enabled = true;
    p.compressor.threshold = 0.5f;
    p.compressor.ratio = 6.0f;
    p.compressor.attackMs = 2.0f;
    p.compressor.releaseMs = 60.0f;

    p.reverb.enabled = true;
    p.reverb.roomSize = 0.6f;
    p.reverb.damping = 0.7f;
    p.reverb.mix = 0.25f;

    p.chorus.enabled = true;
    p.chorus.rate = 1.5f;
    p.chorus.depth = 0.005f;
    p.chorus.mix = 0.3f;
    p.chorus.feedback = 0.2f;
    p.chorus.baseDelay = 0.015f;

    p.distortion.enabled = true;
    p.distortion.mode = DistortionMode::SoftClip;
    p.distortion.drive = 2.0f;
    p.distortion.mix = 0.5f;
    p.distortion.outputGain = 0.6f;
    p.distortion.crushBits = 12.0f;
    p.distortion.crushRate = 0.8f;

    p.eq.enabled = true;
    p.eq.masterGain = 1.5f;
    for (int b = 0; b < 7; b++)
        p.eq.bandGains[b] = (b - 3) * 1.0f;

    e.applyBusPreset(Engine::MASTER_BUS_ID, p);

    // Play a voice through it
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 0.5f);
    e.startVoice(v, 0.0);
    e.renderBlock(4096);
    PASS();
}

TEST(apply_bus_preset_invalid_bus_safe) {
    Engine e; e.initHeadless();
    BusPreset p;
    e.applyBusPreset(9999, p);
    PASS();
}

// ---------------------------------------------------------------------------
// applyModPreset
// ---------------------------------------------------------------------------

TEST(apply_mod_preset_sets_lfos_and_routes) {
    Engine e; e.initHeadless();

    ModPreset p;
    p.lfos[0].shape = LfoShape::Triangle;
    p.lfos[0].rate = 4.0f;
    p.lfos[0].depth = 1.0f;
    p.lfos[0].offset = 0.2f;
    p.lfos[0].bipolar = true;
    p.lfos[0].sync = false;

    p.lfos[1].shape = LfoShape::Square;
    p.lfos[1].rate = 2.0f;

    p.routes.push_back({ModSource::Lfo1, ModDest::Pitch, 1.0f, true});
    p.routes.push_back({ModSource::Lfo2, ModDest::Gain, 0.5f, true});
    p.routes.push_back({ModSource::Envelope, ModDest::FilterFreq, 2.0f, false});

    e.applyModPreset(p);

    // Re-apply to exercise clearAllRoutes path
    p.routes.clear();
    p.routes.push_back({ModSource::Lfo1, ModDest::Pan, 0.3f, true});
    e.applyModPreset(p);

    // Sanity: play a voice
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 0.5f);
    e.startVoice(v, 0.0);
    e.renderBlock(2048);
    PASS();
}

// ---------------------------------------------------------------------------
// applyEnginePreset
// ---------------------------------------------------------------------------

TEST(apply_engine_preset_creates_child_buses) {
    Engine e; e.initHeadless();

    EnginePreset ep;
    ep.masterGain = 0.7f;
    ep.limiter.enabled = true;
    ep.limiter.thresholdDb = -3.0f;
    ep.limiter.releaseMs = 75.0f;

    ep.masterBus.gain = 0.95f;
    ep.masterBus.reverb.enabled = true;
    ep.masterBus.reverb.roomSize = 0.5f;

    BusPreset c1;
    c1.gain = 0.5f;
    c1.delay.enabled = true;
    c1.delay.time = 0.2f;
    BusPreset c2;
    c2.gain = 0.8f;
    c2.chorus.enabled = true;
    ep.buses.push_back(c1);
    ep.buses.push_back(c2);

    ep.modulation.lfos[0].rate = 3.0f;
    ep.modulation.routes.push_back({ModSource::Lfo1, ModDest::Pitch, 1.0f, true});

    e.applyEnginePreset(ep);
    ASSERT_NEAR(e.masterGain(), 0.7f, 1e-3f);

    // Re-apply (now should reuse existing child buses)
    ep.masterGain = 0.4f;
    e.applyEnginePreset(ep);
    ASSERT_NEAR(e.masterGain(), 0.4f, 1e-3f);
    PASS();
}

TEST(apply_engine_preset_with_no_children) {
    Engine e; e.initHeadless();
    EnginePreset ep;
    ep.masterGain = 0.6f;
    // no child buses
    e.applyEnginePreset(ep);
    ASSERT_NEAR(e.masterGain(), 0.6f, 1e-3f);
    PASS();
}

int main() { return runAllTests(); }
