# Building

## Requirements

- C++20 compiler (MSVC 2022, GCC 12+, Clang 15+)
- CMake 3.24+
- SDL3: a consumer's target, else an installed package (`find_package`), else the pinned SDL,
  built static in the tree

Everything else is a dependency pinned in `CMakeLists.txt` and resolved by
`cmake/bro_deps.cmake`: an existing target in the build, else a working tree beside the
top-level project at `../<name>`, else the pinned commit, fetched at configure (override any
with `-DFETCHCONTENT_SOURCE_DIR_<NAME>=<path>`). There are no submodules.

- [`bromath`](https://github.com/wlejon/bromath), header-only math
- [bronze](https://github.com/wlejon/bronze) and, through it, [brass](https://github.com/wlejon/brass),
  for the JavaScript binding. They compile inside the build tree, with the same compiler and CRT.
- libremidi, for MIDI input (`BROAUDIO_MIDI`)

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
subdirectory. A `bromath::bromath` or bronze target that already exists is reused, and pins the
top-level project declared first win over broaudio's own.

## Options

| Option | Default | Description |
|---|---|---|
| `BROAUDIO_MIDI` | `ON` | MIDI input via libremidi (pinned, fetched at configure). Defines `BROAUDIO_HAS_MIDI`. |
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
