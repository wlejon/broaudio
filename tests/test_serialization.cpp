#include "test_harness.h"
#include "broaudio/io/serialization.h"

#include <cstdio>

using namespace broaudio;

// ---------------------------------------------------------------------------
// VoicePreset round-trip
// ---------------------------------------------------------------------------

TEST(voice_preset_round_trip) {
    VoicePreset orig;
    orig.waveform = Waveform::Sawtooth;
    orig.frequency = 880.0f;
    orig.gain = 0.7f;
    orig.pan = -0.3f;
    orig.pitchBend = 2.0f;
    orig.attackTime = 0.05f;
    orig.decayTime = 0.2f;
    orig.sustainLevel = 0.6f;
    orig.releaseTime = 0.5f;
    orig.filterEnabled = true;
    orig.filterType = BiquadFilter::Type::Highpass;
    orig.filterFreq = 2000.0f;
    orig.filterQ = 3.5f;
    orig.unisonCount = 4;
    orig.unisonDetune = 0.25f;
    orig.unisonStereoWidth = 0.9f;

    std::string json = toJson(orig);
    VoicePreset loaded = voicePresetFromJson(json);

    ASSERT_EQ(static_cast<int>(loaded.waveform), static_cast<int>(Waveform::Sawtooth));
    ASSERT_NEAR(loaded.frequency, 880.0f, 1e-4f);
    ASSERT_NEAR(loaded.gain, 0.7f, 1e-4f);
    ASSERT_NEAR(loaded.pan, -0.3f, 1e-4f);
    ASSERT_NEAR(loaded.pitchBend, 2.0f, 1e-4f);
    ASSERT_NEAR(loaded.attackTime, 0.05f, 1e-4f);
    ASSERT_NEAR(loaded.decayTime, 0.2f, 1e-4f);
    ASSERT_NEAR(loaded.sustainLevel, 0.6f, 1e-4f);
    ASSERT_NEAR(loaded.releaseTime, 0.5f, 1e-4f);
    ASSERT_TRUE(loaded.filterEnabled);
    ASSERT_EQ(static_cast<int>(loaded.filterType), static_cast<int>(BiquadFilter::Type::Highpass));
    ASSERT_NEAR(loaded.filterFreq, 2000.0f, 1e-4f);
    ASSERT_NEAR(loaded.filterQ, 3.5f, 1e-4f);
    ASSERT_EQ(loaded.unisonCount, 4);
    ASSERT_NEAR(loaded.unisonDetune, 0.25f, 1e-4f);
    ASSERT_NEAR(loaded.unisonStereoWidth, 0.9f, 1e-4f);

    PASS();
}

// ---------------------------------------------------------------------------
// BusPreset round-trip
// ---------------------------------------------------------------------------

TEST(bus_preset_round_trip) {
    BusPreset orig;
    orig.gain = 0.8f;
    orig.pan = 0.5f;

    // Custom effect order
    orig.effectOrder[0] = EffectSlot::Reverb;
    orig.effectOrder[1] = EffectSlot::Compressor;
    orig.effectOrder[2] = EffectSlot::Filter;
    orig.effectOrder[3] = EffectSlot::Delay;
    orig.effectOrder[4] = EffectSlot::Chorus;
    orig.effectOrder[5] = EffectSlot::Distortion;
    orig.effectOrder[6] = EffectSlot::Equalizer;

    // Filter slot 0
    orig.filters[0].enabled = true;
    orig.filters[0].type = BiquadFilter::Type::Peaking;
    orig.filters[0].frequency = 5000.0f;
    orig.filters[0].Q = 2.0f;
    orig.filters[0].gainDB = 6.0f;

    // Delay
    orig.delay.enabled = true;
    orig.delay.time = 0.4f;
    orig.delay.feedback = 0.6f;
    orig.delay.mix = 0.3f;

    // Compressor
    orig.compressor.enabled = true;
    orig.compressor.threshold = 0.5f;
    orig.compressor.ratio = 8.0f;
    orig.compressor.attackMs = 5.0f;
    orig.compressor.releaseMs = 200.0f;

    // Reverb
    orig.reverb.enabled = true;
    orig.reverb.roomSize = 0.9f;
    orig.reverb.damping = 0.3f;
    orig.reverb.mix = 0.5f;

    // Chorus
    orig.chorus.enabled = true;
    orig.chorus.rate = 2.0f;
    orig.chorus.depth = 0.01f;
    orig.chorus.mix = 0.4f;
    orig.chorus.feedback = 0.3f;
    orig.chorus.baseDelay = 0.02f;

    // Distortion
    orig.distortion.enabled = true;
    orig.distortion.mode = DistortionMode::Foldback;
    orig.distortion.drive = 3.0f;
    orig.distortion.mix = 0.8f;
    orig.distortion.outputGain = 0.5f;
    orig.distortion.crushBits = 8.0f;
    orig.distortion.crushRate = 0.5f;

    // EQ
    orig.eq.enabled = true;
    orig.eq.masterGain = 2.0f;
    orig.eq.bandGains[0] = -3.0f;
    orig.eq.bandGains[3] = 6.0f;
    orig.eq.bandGains[6] = -1.5f;

    std::string json = toJson(orig);
    BusPreset loaded = busPresetFromJson(json);

    ASSERT_NEAR(loaded.gain, 0.8f, 1e-4f);
    ASSERT_NEAR(loaded.pan, 0.5f, 1e-4f);

    // Effect order
    ASSERT_EQ(static_cast<int>(loaded.effectOrder[0]), static_cast<int>(EffectSlot::Reverb));
    ASSERT_EQ(static_cast<int>(loaded.effectOrder[2]), static_cast<int>(EffectSlot::Filter));
    ASSERT_EQ(static_cast<int>(loaded.effectOrder[6]), static_cast<int>(EffectSlot::Equalizer));

    // Filter
    ASSERT_TRUE(loaded.filters[0].enabled);
    ASSERT_EQ(static_cast<int>(loaded.filters[0].type), static_cast<int>(BiquadFilter::Type::Peaking));
    ASSERT_NEAR(loaded.filters[0].frequency, 5000.0f, 1e-4f);
    ASSERT_NEAR(loaded.filters[0].gainDB, 6.0f, 1e-4f);

    // Delay
    ASSERT_TRUE(loaded.delay.enabled);
    ASSERT_NEAR(loaded.delay.time, 0.4f, 1e-4f);
    ASSERT_NEAR(loaded.delay.feedback, 0.6f, 1e-4f);

    // Compressor
    ASSERT_TRUE(loaded.compressor.enabled);
    ASSERT_NEAR(loaded.compressor.ratio, 8.0f, 1e-4f);

    // Reverb
    ASSERT_TRUE(loaded.reverb.enabled);
    ASSERT_NEAR(loaded.reverb.roomSize, 0.9f, 1e-4f);

    // Chorus
    ASSERT_TRUE(loaded.chorus.enabled);
    ASSERT_NEAR(loaded.chorus.rate, 2.0f, 1e-4f);
    ASSERT_NEAR(loaded.chorus.feedback, 0.3f, 1e-4f);

    // Distortion
    ASSERT_TRUE(loaded.distortion.enabled);
    ASSERT_EQ(static_cast<int>(loaded.distortion.mode), static_cast<int>(DistortionMode::Foldback));
    ASSERT_NEAR(loaded.distortion.drive, 3.0f, 1e-4f);

    // EQ
    ASSERT_TRUE(loaded.eq.enabled);
    ASSERT_NEAR(loaded.eq.masterGain, 2.0f, 1e-4f);
    ASSERT_NEAR(loaded.eq.bandGains[0], -3.0f, 1e-4f);
    ASSERT_NEAR(loaded.eq.bandGains[3], 6.0f, 1e-4f);

    PASS();
}

// ---------------------------------------------------------------------------
// ModPreset round-trip
// ---------------------------------------------------------------------------

TEST(mod_preset_round_trip) {
    ModPreset orig;

    orig.lfos[0].shape = LfoShape::Triangle;
    orig.lfos[0].rate = 5.0f;
    orig.lfos[0].depth = 0.8f;
    orig.lfos[0].offset = 0.1f;
    orig.lfos[0].bipolar = false;
    orig.lfos[0].sync = true;

    orig.lfos[2].shape = LfoShape::SampleAndHold;
    orig.lfos[2].rate = 0.25f;

    orig.routes.push_back({ModSource::Lfo1, ModDest::Pitch, 2.0f, true});
    orig.routes.push_back({ModSource::Velocity, ModDest::Gain, 0.5f, true});
    orig.routes.push_back({ModSource::Envelope, ModDest::FilterFreq, 3.0f, false});

    std::string json = toJson(orig);
    ModPreset loaded = modPresetFromJson(json);

    ASSERT_EQ(static_cast<int>(loaded.lfos[0].shape), static_cast<int>(LfoShape::Triangle));
    ASSERT_NEAR(loaded.lfos[0].rate, 5.0f, 1e-4f);
    ASSERT_NEAR(loaded.lfos[0].depth, 0.8f, 1e-4f);
    ASSERT_NEAR(loaded.lfos[0].offset, 0.1f, 1e-4f);
    ASSERT_FALSE(loaded.lfos[0].bipolar);
    ASSERT_TRUE(loaded.lfos[0].sync);

    ASSERT_EQ(static_cast<int>(loaded.lfos[2].shape), static_cast<int>(LfoShape::SampleAndHold));
    ASSERT_NEAR(loaded.lfos[2].rate, 0.25f, 1e-4f);

    ASSERT_EQ(static_cast<int>(loaded.routes.size()), 3);
    ASSERT_EQ(static_cast<int>(loaded.routes[0].source), static_cast<int>(ModSource::Lfo1));
    ASSERT_EQ(static_cast<int>(loaded.routes[0].dest), static_cast<int>(ModDest::Pitch));
    ASSERT_NEAR(loaded.routes[0].amount, 2.0f, 1e-4f);
    ASSERT_TRUE(loaded.routes[0].enabled);

    ASSERT_EQ(static_cast<int>(loaded.routes[1].source), static_cast<int>(ModSource::Velocity));
    ASSERT_EQ(static_cast<int>(loaded.routes[2].dest), static_cast<int>(ModDest::FilterFreq));
    ASSERT_FALSE(loaded.routes[2].enabled);

    PASS();
}

// ---------------------------------------------------------------------------
// EnginePreset round-trip
// ---------------------------------------------------------------------------

TEST(engine_preset_round_trip) {
    EnginePreset orig;
    orig.masterGain = 0.75f;
    orig.limiter.enabled = true;
    orig.limiter.thresholdDb = -3.0f;
    orig.limiter.releaseMs = 80.0f;

    orig.masterBus.gain = 0.9f;
    orig.masterBus.reverb.enabled = true;
    orig.masterBus.reverb.roomSize = 0.7f;

    // Add a child bus
    BusPreset child;
    child.gain = 0.6f;
    child.delay.enabled = true;
    child.delay.time = 0.25f;
    orig.buses.push_back(child);

    // Modulation
    orig.modulation.lfos[0].rate = 3.0f;
    orig.modulation.routes.push_back({ModSource::Lfo1, ModDest::Pan, 0.8f, true});

    std::string json = toJson(orig);
    EnginePreset loaded = enginePresetFromJson(json);

    ASSERT_NEAR(loaded.masterGain, 0.75f, 1e-4f);
    ASSERT_TRUE(loaded.limiter.enabled);
    ASSERT_NEAR(loaded.limiter.thresholdDb, -3.0f, 1e-4f);
    ASSERT_NEAR(loaded.limiter.releaseMs, 80.0f, 1e-4f);

    ASSERT_NEAR(loaded.masterBus.gain, 0.9f, 1e-4f);
    ASSERT_TRUE(loaded.masterBus.reverb.enabled);
    ASSERT_NEAR(loaded.masterBus.reverb.roomSize, 0.7f, 1e-4f);

    ASSERT_EQ(static_cast<int>(loaded.buses.size()), 1);
    ASSERT_NEAR(loaded.buses[0].gain, 0.6f, 1e-4f);
    ASSERT_TRUE(loaded.buses[0].delay.enabled);
    ASSERT_NEAR(loaded.buses[0].delay.time, 0.25f, 1e-4f);

    ASSERT_NEAR(loaded.modulation.lfos[0].rate, 3.0f, 1e-4f);
    ASSERT_EQ(static_cast<int>(loaded.modulation.routes.size()), 1);
    ASSERT_EQ(static_cast<int>(loaded.modulation.routes[0].dest), static_cast<int>(ModDest::Pan));

    PASS();
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

static const char* TEST_PRESET_PATH = "test_preset.json";

TEST(preset_file_save_and_load) {
    VoicePreset orig;
    orig.waveform = Waveform::Square;
    orig.frequency = 660.0f;

    std::string json = toJson(orig);
    bool saved = savePresetToFile(json, TEST_PRESET_PATH);
    ASSERT_TRUE(saved);

    std::string loaded = loadPresetFromFile(TEST_PRESET_PATH);
    ASSERT_FALSE(loaded.empty());

    VoicePreset parsed = voicePresetFromJson(loaded);
    ASSERT_EQ(static_cast<int>(parsed.waveform), static_cast<int>(Waveform::Square));
    ASSERT_NEAR(parsed.frequency, 660.0f, 1e-4f);

    std::remove(TEST_PRESET_PATH);
    PASS();
}

TEST(load_nonexistent_preset_returns_empty) {
    std::string result = loadPresetFromFile("does_not_exist_12345.json");
    ASSERT_TRUE(result.empty());
    PASS();
}

// ---------------------------------------------------------------------------
// Default preset values are preserved
// ---------------------------------------------------------------------------

TEST(default_voice_preset_round_trip) {
    VoicePreset orig;  // all defaults
    std::string json = toJson(orig);
    VoicePreset loaded = voicePresetFromJson(json);

    ASSERT_EQ(static_cast<int>(loaded.waveform), static_cast<int>(Waveform::Sine));
    ASSERT_NEAR(loaded.frequency, 440.0f, 1e-4f);
    ASSERT_NEAR(loaded.gain, 1.0f, 1e-4f);
    ASSERT_NEAR(loaded.pan, 0.0f, 1e-4f);
    ASSERT_FALSE(loaded.filterEnabled);
    ASSERT_EQ(loaded.unisonCount, 1);

    PASS();
}

TEST(default_bus_preset_round_trip) {
    BusPreset orig;  // all defaults
    std::string json = toJson(orig);
    BusPreset loaded = busPresetFromJson(json);

    ASSERT_NEAR(loaded.gain, 1.0f, 1e-4f);
    ASSERT_NEAR(loaded.pan, 0.0f, 1e-4f);
    ASSERT_FALSE(loaded.delay.enabled);
    ASSERT_FALSE(loaded.compressor.enabled);
    ASSERT_FALSE(loaded.reverb.enabled);
    ASSERT_FALSE(loaded.chorus.enabled);
    ASSERT_FALSE(loaded.distortion.enabled);
    ASSERT_FALSE(loaded.eq.enabled);

    PASS();
}

int main() { return runAllTests(); }
