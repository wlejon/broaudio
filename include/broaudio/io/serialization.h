#pragma once

#include "broaudio/types.h"
#include "broaudio/dsp/biquad.h"
#include "broaudio/dsp/params.h"
#include "broaudio/synth/modulation.h"

#include <string>
#include <vector>

namespace broaudio {

// ---------------------------------------------------------------------------
// Preset structs — plain data, no atomics. Used for serialization and for
// programmatically defining patches that can be applied to live objects.
// ---------------------------------------------------------------------------

struct VoicePreset {
    Waveform waveform = Waveform::Sine;
    float frequency = 440.0f;
    float gain = 1.0f;
    float pan = 0.0f;
    float pitchBend = 0.0f;

    // ADSR (times in seconds)
    float attackTime = 0.01f;
    float decayTime = 0.1f;
    float sustainLevel = 1.0f;
    float releaseTime = 0.04f;

    // Per-voice filter
    bool filterEnabled = false;
    BiquadFilter::Type filterType = BiquadFilter::Type::Lowpass;
    float filterFreq = 1000.0f;
    float filterQ = 1.0f;

    // Unison
    int unisonCount = 1;
    float unisonDetune = 0.15f;
    float unisonStereoWidth = 0.7f;
};

struct FilterPreset {
    bool enabled = false;
    BiquadFilter::Type type = BiquadFilter::Type::Lowpass;
    float frequency = 1000.0f;
    float Q = 1.0f;
    float gainDB = 0.0f;
};

struct DelayPreset {
    bool enabled = false;
    float time = 0.3f;
    float feedback = 0.3f;
    float mix = 0.5f;
};

struct CompressorPreset {
    bool enabled = false;
    float threshold = 0.7f;
    float ratio = 4.0f;
    float attackMs = 1.0f;
    float releaseMs = 100.0f;
};

struct ReverbPreset {
    bool enabled = false;
    float roomSize = 0.85f;
    float damping = 0.5f;
    float mix = 0.3f;
};

struct ChorusPreset {
    bool enabled = false;
    float rate = 0.5f;
    float depth = 0.005f;
    float mix = 0.5f;
    float feedback = 0.0f;
    float baseDelay = 0.01f;
};

struct DistortionPreset {
    bool enabled = false;
    DistortionMode mode = DistortionMode::SoftClip;
    float drive = 1.0f;
    float mix = 1.0f;
    float outputGain = 1.0f;
    float crushBits = 16.0f;
    float crushRate = 1.0f;
};

struct EqPreset {
    bool enabled = false;
    float bandGains[7] = {};
    float masterGain = 0.0f;
};

struct BusPreset {
    float gain = 1.0f;
    float pan = 0.0f;

    // Effect processing order
    EffectSlot effectOrder[static_cast<int>(EffectSlot::Count)] = {
        EffectSlot::Filter, EffectSlot::Delay, EffectSlot::Compressor,
        EffectSlot::Chorus, EffectSlot::Reverb, EffectSlot::Equalizer,
        EffectSlot::Distortion
    };

    // Per-effect presets
    static constexpr int MAX_FILTERS = 4;
    FilterPreset filters[MAX_FILTERS];
    DelayPreset delay;
    CompressorPreset compressor;
    ReverbPreset reverb;
    ChorusPreset chorus;
    DistortionPreset distortion;
    EqPreset eq;
};

struct LfoPreset {
    LfoShape shape = LfoShape::Sine;
    float rate = 1.0f;
    float depth = 1.0f;
    float offset = 0.0f;
    bool bipolar = true;
    bool sync = false;
};

struct RoutePreset {
    ModSource source = ModSource::Lfo1;
    ModDest dest = ModDest::Pitch;
    float amount = 0.0f;
    bool enabled = true;
};

struct ModPreset {
    static constexpr int MAX_LFOS = 4;
    LfoPreset lfos[MAX_LFOS];
    std::vector<RoutePreset> routes;
};

struct LimiterPreset {
    bool enabled = true;
    float thresholdDb = -6.0f;
    float releaseMs = 50.0f;
};

struct EnginePreset {
    float masterGain = 0.5f;
    LimiterPreset limiter;
    BusPreset masterBus;
    std::vector<BusPreset> buses;  // child buses (index 0 = first child, not master)
    ModPreset modulation;
};

// ---------------------------------------------------------------------------
// Serialization — to/from JSON strings
// ---------------------------------------------------------------------------

std::string toJson(const VoicePreset& preset);
std::string toJson(const BusPreset& preset);
std::string toJson(const ModPreset& preset);
std::string toJson(const EnginePreset& preset);

VoicePreset voicePresetFromJson(const std::string& json);
BusPreset busPresetFromJson(const std::string& json);
ModPreset modPresetFromJson(const std::string& json);
EnginePreset enginePresetFromJson(const std::string& json);

// ---------------------------------------------------------------------------
// File convenience
// ---------------------------------------------------------------------------

bool savePresetToFile(const std::string& json, const char* path);
std::string loadPresetFromFile(const char* path);

} // namespace broaudio
