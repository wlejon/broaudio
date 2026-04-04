#pragma once

#include <cstdint>

namespace broaudio {

enum class Waveform : uint8_t { Sine, Square, Sawtooth, Triangle };

enum class EnvStage : uint8_t { Idle, Attack, Decay, Sustain, Release, Done };

enum class DistanceModel : uint8_t {
    Linear,
    Inverse,
    Exponential
};

} // namespace broaudio
