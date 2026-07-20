# broaudio

[![CI](https://github.com/wlejon/broaudio/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/broaudio/actions/workflows/ci.yml)
[![CodeQL](https://github.com/wlejon/broaudio/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/broaudio/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A real-time audio engine library written in C++20. Provides synthesis, sample playback, streaming sources, effects processing, spatial audio, MIDI input, audio-file decode/encode, system-audio capture, and a flexible mixing bus architecture — all with a lock-free, audio-thread-safe design built on SDL3.

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
- **Solo** — `setBusSolo` silences non-soloed bus outputs; soloed subtrees and the ancestors carrying their audio stay audible, mute wins over solo, and sources routed directly to an audible ancestor are untouched (Godot semantics)
- **Aux sends** — From voices, clips, or buses to any other bus
- **Bus routing** — Voices and clips can target any bus
- **Bus metering** — Per-bus peak and RMS (L/R) for level visualization

### Audio Clips
- **Sample playback** — Load from raw float buffers (mono or stereo), play with gain/pan/rate control
- **Looping and regions** — Loop points and sub-region playback
- **Variable-rate playback** — Time-stretching via playback rate
- **Scheduled start** — `playClipAt` begins a clip when the audio clock reaches a given engine time, with no main-thread timer jitter
- **Seek and position** — `seekPlayback` jumps the cursor; `getPlaybackPosition` (normalized) and `getPlaybackPositionSeconds` report it
- **Waveform extraction** — Min/max binning for display
- **Decode from file** — `createClipFromFile` loads WAV/FLAC/MP3/OGG straight into a clip; `createClipFromFileEx` reports an actionable error, `createClipFromFileAsync` decodes on a background thread, and `setMaxClipDecodedBytes` caps decoded size (default ~200 MB)

### Streaming Sources
- **Live PCM streams** — `createStream` returns a persistent playback fed frame-by-frame via `pushStreamSamples`; the audio thread reads a ring the producer appends to, so network or voice audio plays click-free without per-chunk clip churn
- **Disk-streamed files** — `createStreamFromFile` plays large files without decoding them into RAM: a worker thread decodes and resamples incrementally into the same ring, with configurable ring size, prebuffer, and seamless looping
- **Full playback control** — Streaming instances accept every `setPlayback*` / `setPlaybackSpatial*` call (gain, pan, bus, sends, 3D position); disk streams also support `seekPlayback`
- **Ring statistics** — `getStreamStats` reports decoded/played/buffered frames, underruns, and EOF

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
- **Doppler** — Per-source and listener velocities with a global `setDopplerFactor`; clips compose the ratio into their resampling rate, voices fold it into pitch. Ratio is clamped to [0.5, 2.0]; streaming playbacks are not shifted (their ring mixer has no resampler)

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
- **Mic taps** — Multi-consumer audio-thread mic dispatch; each tap requests its own target rate (polyphase resampled), fixed chunk size, and optional AGC, with per-tap stats. Single hook for wake-word detectors, live ASR, custom analyzers. `injectMicSamples` drives the same tap chain from synthetic audio for headless and test use
- **Output recording** — Capture up to 60 seconds of engine output

### System-Audio Capture
`LoopbackCapture` captures what the machine is *playing* (the render side), independent of the engine's mic path. Frames arrive as FP32 on the capture thread, optionally downmixed to mono and resampled to a target rate.

- **Modes** — `SystemOutput` (the whole render endpoint), `ProcessInclude` / `ProcessExclude` (one application's process tree, or everything but it)
- **Backends** — Windows WASAPI loopback; macOS CoreAudio process taps (14.2+, needs `NSAudioCaptureUsageDescription`); Linux PulseAudio monitor source via libpulse (also covers pipewire-pulse); a stub elsewhere
- **Graceful degradation** — `isSupported()` is false where unavailable, and the process modes report unsupported on PulseAudio (no public per-app capture API); `enumerateProcesses()` still lists apps holding a render session, for a picker

### Engine Control and Offline Rendering
- **Headless mode** — `initHeadless()` opens no device; `renderBlock(numFrames)` drives the full pipeline (voices, clips, bus FX, mix, limiter, metering, recording) on the calling thread
- **Master pause** — `setMasterPaused` suspends the output clock: the graph stops advancing and resumes exactly where it left off, rather than muting
- **Output latency** — `outputLatencySeconds()` estimates device-buffer latency (a lower bound; OS mixer and DAC latency are not visible)
- **Sample-accurate events** — `scheduleNoteOn` / `scheduleNoteOff` dispatch inside the audio callback at a given engine time
- **Offline effects** — `processEffectsOffline` runs mono samples through a clone of a bus's effect chain without touching live audio

## Architecture

The engine uses a **lock-free, RCU-based design** for thread safety between the main thread and the audio callback:

- **Voices, clips, playbacks, and buses** are managed via `std::atomic<std::shared_ptr<const Vector>>` (C++20) — the main thread publishes new lists atomically; the audio thread reads the current snapshot without locking.
- **Per-object parameters** (gain, frequency, filter settings, etc.) use `std::atomic` with relaxed ordering for individual fields, and version counters to batch-apply parameter changes on the audio thread.
- **Pre-allocated scratch buffers** prevent heap allocations on the audio thread.

```
Voices  ──┐
Clips   ──┤
Streams ──┼──▶ Bus (child) ──▶ Bus (master) ──▶ Limiter ──▶ Output
Mic     ──┘         │                │
                   FX chain         FX chain
              (filter/delay/    (filter/delay/
               comp/chorus/      comp/chorus/
               distortion/       distortion/
               reverb/EQ)        reverb/EQ)
```

The per-bus FX chain order is configurable via `setBusEffectOrder`. Streaming sources (live PCM and disk-streamed files) are fed by producer threads through a ring the audio thread drains; file decoding and resampling happen on a dedicated worker, never in the callback.

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
Each test is a standalone executable registered with CTest (no aggregate target), built on the header-only harness in `tests/test_harness.h`.

```bash
cmake --build build
ctest --test-dir build
```

Set `SDL_AUDIODRIVER=dummy` to run without an audio device (this is what CI does).

Coverage: CI runs a Debug `--coverage` build through gcovr on Linux and uploads an HTML report. On Windows, `pwsh scripts/coverage.ps1` produces the equivalent via OpenCppCoverage from a Debug build.

### Repository layout
| Path | Contents |
|---|---|
| `include/broaudio/` | Public headers — `engine.h` is the primary entry point |
| `src/` | Implementation, mirroring the header layout |
| `tests/` | One executable per test, registered with CTest |
| `third_party/` | Vendored dr_libs, stb_vorbis, nlohmann; libremidi as a submodule |
| `scripts/` | `coverage.ps1` (Windows coverage report) |

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

// Stream a large file from disk instead of decoding it into RAM
std::string err;
int music = engine.createStreamFromFile("song.flac", &err);
if (music >= 0) engine.setPlaybackGain(music, 0.6f);
```

Call `engine.update()` from your frame loop to run non-realtime maintenance (reaping finished voices, servicing async work).

## License

[MIT](LICENSE)
