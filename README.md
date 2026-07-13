# broaudio

[![CI](https://github.com/wlejon/broaudio/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/broaudio/actions/workflows/ci.yml)
[![CodeQL](https://github.com/wlejon/broaudio/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/broaudio/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A real-time audio engine library written in C++20. Provides synthesis, sample playback, effects processing, spatial audio, MIDI input, audio-file decode/encode, and a flexible mixing bus architecture — all with a lock-free, audio-thread-safe design built on SDL3.

## Features

### Synthesis
- **Oscillators** — Sine, square, sawtooth, triangle with polyBLEP anti-aliasing
- **Wavetable** — Mipmap-per-octave wavetable bank with cubic interpolation; factory presets (saw, square, triangle) and arbitrary waveform import
- **Noise** — White, pink (Voss-McCartney), and brown noise generators
- **ADSR envelope** — Per-voice attack/decay/sustain/release
- **Per-voice filter** — Biquad with 8 types (LP, HP, BP, notch, allpass, peaking, low/high shelf)
- **Pitch bend and pan** — Per-voice control
- **Unison / Supersaw** — 1-8 sub-oscillators per voice with configurable detune spread and stereo width; gain-normalized by `1/sqrt(N)`

### Voice Management
- **Voice allocator** — Configurable polyphony (default 16 voices)
- **Steal policies** — Oldest, quietest, same-note, or none (drop)
- **Voice setup callback** — Configure waveform, ADSR, bus routing, etc. on allocation

### Modulation
- **4 LFOs** — Sine, triangle, square, saw up/down, sample-and-hold; per-voice phase with optional note-on sync
- **Modulation matrix** — Up to 16 routes
- **Sources** — LFO 1-4, envelope, velocity, key tracking, mod wheel, aftertouch
- **Destinations** — Pitch, gain, pan, filter frequency, filter Q, pulse width, delay send

### Effects (per-bus, configurable order)
- **Configurable effect chain** — Reorder the processing chain per bus via `setBusEffectOrder`
- **Biquad filters** — Up to 4 slots per bus, 8 filter types
- **Delay** — Feedback delay with wet/dry mix
- **Compressor** — Threshold, ratio, attack, release, with optional **sidechain** from another bus
- **Reverb** — Freeverb-style stereo algorithmic reverb (8 comb + 4 allpass)
- **Chorus/Flanger** — Modulated stereo delay lines
- **Distortion / Waveshaper** — Soft clip (tanh), hard clip, foldback, and bitcrush modes with drive, mix, and output gain
- **Equalizer** — 7-band parametric EQ (60 Hz–16 kHz) with presets
- **Master limiter** — Lookahead peak limiter on the master bus

### Mixing
- **Bus hierarchy** — Master bus + dynamically created child buses
- **Per-bus controls** — Gain, pan, mute
- **Aux sends** — From voices, clips, or buses to any other bus
- **Bus routing** — Voices and clips can target any bus
- **Bus metering** — Per-bus peak and RMS (L/R) for level visualization

### Audio Clips
- **Sample playback** — Load from raw float buffers (mono or stereo), play with gain/pan/rate control
- **Looping and regions** — Loop points and sub-region playback
- **Variable-rate playback** — Time-stretching via playback rate
- **Waveform extraction** — Min/max binning for display
- **Decode from file** — `createClipFromFile` loads WAV/FLAC/MP3/OGG straight into a clip

### Audio File I/O
- **Decode** — `loadAudioFile` / `loadAudioFileFromMemory` decode WAV, FLAC, MP3, and OGG (Opus requires `BROAUDIO_OPUS`) to interleaved float32 PCM (via dr_libs)
- **Encode** — `saveWav` writes 32-bit float WAV; `exportRecordingToWav` dumps captured output
- **Offline resampling** — Polyphase Kaiser-windowed sinc `resample()` for arbitrary-ratio sample-rate conversion

### Spatial Audio
- **3D listener** — Position and orientation (forward + up vectors)
- **Per-source positioning** — On both voices and clip playback instances
- **Distance models** — Linear, inverse, exponential with configurable ref/max distance and rolloff
- **Angle-based stereo panning** — Projects source direction onto listener's right vector
- **Head-shadow model** — Optional HRTF-style ILD (interaural level difference) plus per-ear one-pole lowpass for front/back and elevation cues, all caller-tunable

### MIDI
- **Hardware/virtual port input** via libremidi
- **Lock-free event delivery** — SPSC ring buffer from callback to main thread
- **VoiceAllocator integration** — Automatic note on/off routing
- **CC mapping** — Register per-CC callbacks
- **Pitch bend and aftertouch** — Dedicated callbacks

### Sequencer
- **Beat-based timeline** — Schedule note events at beat positions with duration
- **Tempo control** — Configurable BPM and time signature
- **Transport** — Play, stop, pause, resume
- **Looping** — Loop range with automatic wrap-around
- **Automation lanes** — Per-parameter breakpoint envelopes with step/linear/smooth interpolation, applied each `update`
- **VoiceAllocator integration** — Fires noteOn/noteOff at sample-accurate engine time
- **Main-thread driven** — Call `update(engineTime)` from your frame loop

### Presets and Serialization
- **Plain-data preset structs** — `VoicePreset`, `BusPreset`, `ModPreset`, `EnginePreset` describe a full patch with no atomics
- **Apply to live objects** — `applyVoicePreset` / `applyBusPreset` / `applyModPreset` / `applyEnginePreset`
- **JSON round-trip** — `toJson` / `*FromJson` plus `savePresetToFile` / `loadPresetFromFile`

### Analysis and Recording
- **Output and mic analysis buffers** — Ring buffers for visualization
- **FFT spectrum** — Cooley-Tukey, up to 8192 bins
- **Microphone capture** — With monitor gain, mute, and bus routing
- **Mic taps** — Multi-consumer audio-thread mic dispatch; each tap requests its own target rate (polyphase resampled), fixed chunk size, and optional AGC, with per-tap stats. Single hook for wake-word detectors, live ASR, custom analyzers
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
               comp/chorus/        comp/chorus/
               distortion/         distortion/
               reverb/EQ)          reverb/EQ)
```

The per-bus FX chain order is configurable via `setBusEffectOrder`.

## Building

### Requirements
- C++20 compiler (MSVC 2022, GCC 12+, Clang 15+)
- CMake 3.24+
- SDL3
- [`bromath`](https://github.com/wlejon/bromath) — header-only math sibling, expected at `../bromath` (or provide a `bromath::bromath` target)

MIDI support pulls in `libremidi` as a submodule under `third_party/`; initialize submodules (`git submodule update --init`) for the default MIDI-enabled build.

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

The consumer must provide an SDL3 target (`SDL3::SDL3` or `SDL3::SDL3-static`) before adding the subdirectory. If a `bromath::bromath` target already exists in the build it is reused; otherwise broaudio adds the `../bromath` sibling.

### Options
| Option | Default | Description |
|---|---|---|
| `BROAUDIO_MIDI` | `ON` | Enable MIDI input via libremidi (submodule in `third_party/`); defines `BROAUDIO_HAS_MIDI` |
| `BROAUDIO_OPUS` | `OFF` | Enable OGG Opus decoding via opusfile (found via pkg-config / find_package); defines `BROAUDIO_HAS_OPUS` |
| `BROAUDIO_TESTS` | `ON` | Build test suite (standalone builds only) |

If MIDI or Opus dependencies are not found, the respective option is auto-disabled with a status message rather than failing the configure.

### Running tests
Each test is a standalone executable registered with CTest (no aggregate target).

```bash
cmake --build build
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

[MIT](LICENSE)
