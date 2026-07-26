# Building

## Requirements

- C++20 compiler (MSVC 2022, GCC 12+, Clang 15+)
- CMake 3.24+
- SDL3
- [`bromath`](https://github.com/wlejon/bromath), a header-only sibling expected at `../bromath`, or
  a `bromath::bromath` target already in the build

MIDI support pulls in libremidi as a submodule under `third_party/`. Run
`git submodule update --init` for the default MIDI-enabled build.

## Standalone

```bash
cmake -B build
cmake --build build
```

## As a subdirectory

```cmake
add_subdirectory(broaudio)
target_link_libraries(your_app PRIVATE broaudio)
```

The consumer must provide an SDL3 target (`SDL3::SDL3` or `SDL3::SDL3-static`) before adding the
subdirectory. If a `bromath::bromath` target already exists it is reused; otherwise broaudio adds
the `../bromath` sibling.

## Options

| Option | Default | Description |
|---|---|---|
| `BROAUDIO_MIDI` | `ON` | MIDI input via libremidi (submodule in `third_party/`). Defines `BROAUDIO_HAS_MIDI`. |
| `BROAUDIO_OPUS` | `OFF` | OGG Opus decoding via opusfile (pkg-config or find_package). Defines `BROAUDIO_HAS_OPUS`. |
| `BROAUDIO_TESTS` | `ON` | Build the test suite (standalone builds only). |

If MIDI or Opus dependencies are missing, the option auto-disables with a status message rather
than failing the configure.

## Tests

Each test is a standalone executable registered with CTest (there is no aggregate target), built on
the header-only harness in `tests/test_harness.h`.

```bash
cmake --build build
ctest --test-dir build
```

Set `SDL_AUDIODRIVER=dummy` to run without an audio device. That is what CI does.

## Coverage

CI runs a Debug `--coverage` build through gcovr on Linux and uploads an HTML report. On Windows,
`pwsh scripts/coverage.ps1` produces the equivalent via OpenCppCoverage from a Debug build.
