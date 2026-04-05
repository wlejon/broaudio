#pragma once

#include <cstdint>

namespace broaudio {

enum class Waveform : uint8_t {
    Sine, Square, Sawtooth, Triangle, Wavetable,
    WhiteNoise, PinkNoise, BrownNoise
};

enum class EnvStage : uint8_t { Idle, Attack, Decay, Sustain, Release, Done };

enum class DistanceModel : uint8_t {
    Linear,
    Inverse,
    Exponential
};

enum class EffectSlot : uint8_t {
    Filter,
    Delay,
    Compressor,
    Chorus,
    Reverb,
    Equalizer,
    Distortion,
    Count
};

} // namespace broaudio
