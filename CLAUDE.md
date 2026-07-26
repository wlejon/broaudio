# CLAUDE.md

Guidance for working in this repository.

## What this is

`broaudio` is a C++20 real-time audio engine built as a **static library**, not an application. It
is consumed by sibling projects via `add_subdirectory`. Everything here should stay
general-purpose: fix and extend the library rather than working around a limitation in consumer
code.

`Engine` (`include/broaudio/engine.h`) is the primary entry point and the widest surface: voices,
buses, clips, streams, spatial, mic, recording, and preset application all hang off it.

## Build and test

```bash
cmake -B build
cmake --build build
ctest --test-dir build            # SDL_AUDIODRIVER=dummy for no audio device
```

Dependencies: SDL3 (`find_package`, or a consumer-provided `SDL3::SDL3` / `SDL3::SDL3-static`
target), the header-only sibling [`bromath`](https://github.com/wlejon/bromath) expected at
`../bromath`, and libremidi as a submodule under `third_party/`
(`git submodule update --init`).

Optional dependencies degrade rather than fail: if libremidi or opusfile is missing,
`BROAUDIO_MIDI` / `BROAUDIO_OPUS` auto-disable with a status message. Preserve that behavior when
adding dependencies.

Note for MSVC: `src/engine.cpp` is pinned to `/O1` in non-Debug configs to work around a compiler
ICE at `/O2`.

## Threading model, the central constraint

The engine is hard-realtime on the audio thread. [docs/architecture.md](docs/architecture.md)
describes the mechanisms (RCU snapshots, atomic parameter structs, producer rings). The rules that
matter when changing code:

- **Never on the audio thread:** locks, allocation, file or network I/O, blocking of any kind.
  Scratch buffers are pre-allocated; keep them that way.
- **The mutexes in the codebase serialize writers only.** `voiceWriteMutex_`, `busWriteMutex_`,
  `mediaWriteMutex_`, `fileStreamsMutex_`, and the ones inside `FileStreamRunner` are the
  single-writer half of the RCU protocol, held on non-audio threads. If a change would put a lock
  on the callback path, restructure it as an atomic, a ring, or an RCU publish instead.
- **`Engine::update()` must keep being tickable from a non-audio thread.** RCU reclamation and
  async work run there.
- **Off-thread producers keep their contracts:** `pushStreamSamples` is single-producer, mic taps
  and loopback callbacks do realtime-safe work only, `addMicTap` / `removeMicTap` are
  single-writer.

## Layout

| Path | Contents |
|---|---|
| `include/broaudio/` | Public headers; the directory structure mirrors `src/` |
| `src/` | Implementation |
| `src/loopback/` | Per-platform system-audio capture, selected in CMake (`capture_win.cpp`, `capture_macos.mm`, `capture_pulse.cpp`, `capture_null.cpp`) |
| `src/codec/` | Vendored decoder implementation TUs, isolated in the `broaudio_codecs` target so CodeQL can pre-build them out of the Security tab |
| `tests/` | One executable per test file |
| `third_party/` | dr_libs, stb_vorbis, nlohmann (vendored); libremidi (submodule) |
| `docs/` | Feature list, architecture, build guide |

## Conventions

- **Headers carry the contract.** Non-obvious behavior (threading rules, RT-safety, clamping,
  degradation, platform limits) goes in a comment above the declaration in the header, not in a
  separate document. The comments in `engine.h` around solo, master pause, Doppler, and the mic
  taps are the model. `docs/` covers cross-cutting design and orientation, not per-call behavior.
- **Platform gaps degrade gracefully.** An unsupported backend reports `isSupported() == false` and
  fails `start()` cleanly so callers can hide the feature. It does not throw, assert, or log an
  error every block.
- **Errors as return values.** Failure is `-1` or `false`, optionally with a `std::string* outError`
  overload carrying an actionable message. No exceptions on the API surface.
- **Tests** use the dependency-free harness in `tests/test_harness.h` (`TEST(name)` with `ASSERT_*`
  and `PASS()`, plus `int main() { return runAllTests(); }`). Add a test by dropping
  `tests/test_foo.cpp` and appending `test_foo` to the `BROAUDIO_TESTS` list in
  `tests/CMakeLists.txt`. Prefer `initHeadless()` + `renderBlock()` for engine-level tests so they
  run without a device.
