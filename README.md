# broaudio

[![CI](https://github.com/wlejon/broaudio/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/broaudio/actions/workflows/ci.yml)
[![CodeQL](https://github.com/wlejon/broaudio/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/broaudio/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A real-time audio engine library in C++20, built on SDL3 with a lock-free, audio-thread-safe
design. It covers synthesis, sample playback, streaming sources, effects, spatial audio, MIDI,
audio-file decode and encode, system-audio capture, and a hierarchical mixing bus.

## Documentation

| Document | Contents |
|---|---|
| [Features](docs/features.md) | Full capability list by subsystem |
| [Architecture](docs/architecture.md) | Signal flow, threading model, producer paths |
| [Building](docs/building.md) | Requirements, CMake options, tests, coverage |

`Engine` (`include/broaudio/engine.h`) is the primary entry point. Per-call contracts, including
threading rules and clamping, are documented in the headers.

## Quick start

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

Call `engine.update()` from your frame loop to run non-realtime maintenance: reaping finished
voices, servicing async work, and reclaiming retired RCU snapshots.

## Build

```bash
cmake -B build
cmake --build build
ctest --test-dir build
```

Requires a C++20 compiler, CMake 3.24+, SDL3, and the header-only sibling
[`bromath`](https://github.com/wlejon/bromath) at `../bromath`. To consume it from another project:

```cmake
add_subdirectory(broaudio)
target_link_libraries(your_app PRIVATE broaudio)
```

See [docs/building.md](docs/building.md) for options, optional dependencies, and coverage.

## Repository layout

| Path | Contents |
|---|---|
| `include/broaudio/` | Public headers, mirroring `src/` |
| `src/` | Implementation |
| `tests/` | One executable per test, registered with CTest |
| `third_party/` | Vendored dr_libs, stb_vorbis, nlohmann; libremidi as a submodule |
| `scripts/` | `coverage.ps1`, the Windows coverage report |
| `docs/` | Feature list, architecture, build guide |

## License

[MIT](LICENSE)
