# broaudio

A real-time audio engine library written in C++20. Provides synthesis, sample playback, effects processing, spatial audio, MIDI input, and a flexible mixing bus architecture — all with a lock-free, audio-thread-safe design built on SDL3.

## Features

### Synthesis
- **Oscillators** — Sine, square, sawtooth, triangle with polyBLEP anti-aliasing
- **Wavetable** — Mipmap-per-octave wavetable bank with cubic interpolation; factory presets (saw, square, triangle) and arbitrary waveform import
- **Noise** — White, pink (Voss-McCartney), and brown noise generators
- **ADSR envelope** — Per-voice attack/decay/sustain/release
- **Per-voice filter** — Biquad with 8 types (LP, HP, BP, notch, allpass, peaking, low/high shelf)
- **Pitch bend and pan** — Per-voice control

### Voice Management
- **Voice allocator** — Configurable polyphony (default 16 voices)
- **Steal policies** — Oldest, quietest, same-note, or none (drop)
- **Voice setup callback** — Configure waveform, ADSR, bus routing, etc. on allocation

### Modulation
- **4 LFOs** — Sine, triangle, square, saw up/down, sample-and-hold; per-voice phase with optional note-on sync
- **Modulation matrix** — Up to 16 routes
- **Sources** — LFO 1-4, envelope, velocity, key tracking, mod wheel, aftertouch
- **Destinations** — Pitch, gain, pan, filter frequency, filter Q, pulse width, delay send

### Effects (per-bus)
- **Biquad filters** — Up to 4 slots per bus, 8 filter types
- **Delay** — Feedback delay with wet/dry mix
- **Compressor** — Threshold, ratio, attack, release
- **Reverb** — Freeverb-style stereo algorithmic reverb (8 comb + 4 allpass)
- **Chorus/Flanger** — Modulated stereo delay lines
- **Equalizer** — 7-band parametric EQ with presets
- **Master limiter** — Lookahead peak limiter on the master bus

### Mixing
- **Bus hierarchy** — Master bus + dynamically created child buses
- **Per-bus controls** — Gain, pan, mute
- **Aux sends** — From voices, clips, or buses to any other bus
- **Bus routing** — Voices and clips can target any bus

### Audio Clips
- **Sample playback** — Load from raw float buffers, play with gain/pan/rate control
- **Looping and regions** — Loop points and sub-region playback
- **Variable-rate playback** — Time-stretching via playback rate
- **Waveform extraction** — Min/max binning for display

### Spatial Audio
- **3D listener** — Position and orientation (forward + up vectors)
- **Per-source positioning** — On both voices and clip playback instances
- **Distance models** — Linear, inverse, exponential with configurable ref/max distance and rolloff
- **Angle-based stereo panning** — Projects source direction onto listener's right vector

### MIDI
- **Hardware/virtual port input** via libremidi
- **Lock-free event delivery** — SPSC ring buffer from callback to main thread
- **VoiceAllocator integration** — Automatic note on/off routing
- **CC mapping** — Register per-CC callbacks
- **Pitch bend and aftertouch** — Dedicated callbacks

### Analysis and Recording
- **Output and mic analysis buffers** — Ring buffers for visualization
- **FFT spectrum** — Radix-2 Cooley-Tukey, up to 8192 bins
- **Microphone capture** — With monitor gain and mute
- **Output recording** — Capture up to 60 seconds of engine output

## Architecture

The engine uses a **lock-free, RCU-based design** for thread safety between the main thread and the audio callback:

- **Voices, clips, playbacks, and buses** are managed via `std::atomic<std::shared_ptr<const Vector>>` (C++20) — the main thread publishes new lists atomically; the audio thread reads the current snapshot without locking.
- **Per-object parameters** (gain, frequency, filter settings, etc.) use `std::atomic` with relaxed ordering for individual fields, and version counters to batch-apply parameter changes on the audio thread.
- **Pre-allocated scratch buffers** prevent heap allocations on the audio thread.

```
Voices ──┐
          ├──▶ Bus (child) ──▶ Bus (master) ──▶ Limiter ──▶ Output
Clips  ──┘         │                │
                   FX chain        FX chain
              (filter/delay/      (filter/delay/
               comp/reverb/        comp/reverb/
               chorus)             chorus)
```

## Building

### Requirements
- C++20 compiler (MSVC 2022, GCC 12+, Clang 15+)
- CMake 3.24+
- SDL3

### As a standalone project
```bash
cmake -B build
cmake --build build
```

### As a subdirectory
```cmake
add_subdirectory(broaudio)
target_link_libraries(your_app PRIVATE broaudio)
```

The consumer must provide an SDL3 target (`SDL3::SDL3` or `SDL3::SDL3-static`) before adding the subdirectory.

### Options
| Option | Default | Description |
|---|---|---|
| `BROAUDIO_MIDI` | `ON` | Enable MIDI input via libremidi (bundled in `third_party/`) |
| `BROAUDIO_TESTS` | `ON` | Build test suite (standalone builds only) |

### Running tests
```bash
cmake --build build --target broaudio_tests
ctest --test-dir build
```

## Quick Start

```cpp
#include <broaudio/engine.h>

broaudio::Engine engine;
engine.init();

// Create a voice and play a 440 Hz sine
int voice = engine.createVoice();
engine.setWaveform(voice, broaudio::Waveform::Sine);
engine.setFrequency(voice, 440.0f);
engine.setGain(voice, 0.5f);
engine.startVoice(voice, engine.currentTime());

// Play a sample clip
int clip = engine.createClip(samples, numSamples);
int inst = engine.playClip(clip, 0.8f, /*loop=*/true);

// Route a voice through a reverb bus
int reverbBus = engine.createBus();
engine.setBusReverbEnabled(reverbBus, true);
engine.setBusReverbMix(reverbBus, 0.4f);
engine.setVoiceSend(voice, reverbBus, 0.5f);
```

## License

Private. All rights reserved.
