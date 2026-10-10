cmake_minimum_required(VERSION 3.24)
# C is required for the vendored stb_vorbis translation unit
# (src/codec/stb_vorbis_impl.c does not compile as C++).
project(broaudio LANGUAGES CXX C)

# When built standalone, set the MSVC runtime to static
if(MSVC AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Dependencies (cmake/bro_deps.cmake): an outer project's target wins (under bro
# these are all bro's), else the ../<name> working tree beside the top-level
# project, else the head of its main, fetched at configure.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bro_deps.cmake)

# SDL3: the consumer's target, else an installed SDL3 (find_package), else the
# commit bro pins, built static here.
if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
    find_package(SDL3 QUIET)
    if(NOT SDL3_FOUND)
        bro_dependency(SDL GITHUB libsdl-org/SDL REF 1df279a04f604bc50b6f36c9e903b5e64c4fe2a3 THIRD_PARTY
                       TARGET SDL3-static OPTIONS SDL_SHARED=OFF SDL_STATIC=ON SDL_TEST_LIBRARY=OFF)
    endif()
endif()

# bromath, header-only math.
bro_dependency(bromath GITHUB wlejon/bromath TARGET bromath::bromath)

# bronze, the JavaScript runtime the api binds (it brings its code generator,
# brass). The api is bound across bronze's C++ embed boundary, so it has to
# build against the bronze the surrounding tree builds against.
bro_dependency(bronze GITHUB wlejon/bronze TARGET bronze_runtime_shared
               OPTIONS BRONZE_BUILD_SHARED_RUNTIME=ON BRONZE_BUILD_TESTS=OFF)

# libopus — optional Ogg Opus decoding (broaudio's own Ogg demuxer,
# src/codec/ogg_opus.cpp, over libopus; no opusfile). Uses the consumer's
# Opus::opus target when it has one, else an installed Opus CMake package
# (vcpkg's `opus`); without either it switches itself off and Ogg Opus files
# fail with a clear error.
# (BROAUDIO_OPUS was the old opusfile switch, default OFF; the new name keeps
# a stale cache entry from turning this off.)
option(BROAUDIO_WITH_OPUS "Ogg Opus decoding over libopus (auto-off when libopus is absent)" ON)
set(BROAUDIO_HAS_LIBOPUS OFF)
if(BROAUDIO_WITH_OPUS)
    if(NOT TARGET Opus::opus)
        find_package(Opus CONFIG QUIET)
    endif()
    if(NOT TARGET Opus::opus)
        message(STATUS "broaudio: libopus (Opus::opus) not found, Ogg Opus support disabled")
    else()
        set(BROAUDIO_HAS_LIBOPUS ON)
        message(STATUS "broaudio: Ogg Opus decoding via libopus")
    endif()
endif()

# libremidi — optional MIDI support
option(BROAUDIO_MIDI "Enable MIDI input via libremidi" ON)
if(BROAUDIO_MIDI)
    bro_dependency(libremidi GITHUB celtera/libremidi REF 6e8dccd92386311413d8d737ef223dfb71a2d0de THIRD_PARTY
                   TARGET libremidi::libremidi
                   OPTIONS LIBREMIDI_NO_JACK=ON LIBREMIDI_NO_PIPEWIRE=ON LIBREMIDI_NO_NETWORK=ON
                           LIBREMIDI_NO_KEYBOARD=ON)
    if(NOT TARGET libremidi::libremidi)
        set(BROAUDIO_MIDI OFF)
        message(STATUS "broaudio: libremidi not found, MIDI support disabled")
    endif()
endif()

# Vendored single-header codec implementations (dr_wav/dr_flac/dr_mp3 and
# stb_vorbis) live in their own static library so CodeQL's manual build can
# pre-build this target before it starts tracing (see .github/workflows/codeql.yml).
# That build-order step keeps the vendored implementation bodies out of the
# broaudio Security tab; paths-ignore cannot, because it does not filter compiled
# languages built with build-mode: manual.
add_library(broaudio_codecs STATIC
    src/codec/stb_vorbis_impl.c
    src/codec/dr_libs_impl.cpp
    src/codec/minimp4_impl.c
)
target_include_directories(broaudio_codecs PRIVATE
    third_party/dr_libs
    third_party)
if(MSVC)
    target_compile_options(broaudio_codecs PRIVATE /MP /utf-8)
endif()

add_library(broaudio STATIC
    src/core/ring_buffer.cpp
    src/dsp/biquad.cpp
    src/dsp/chorus.cpp
    src/dsp/compressor.cpp
    src/dsp/convolution_reverb.cpp
    src/dsp/delay.cpp
    src/dsp/distortion.cpp
    src/dsp/equalizer.cpp
    src/dsp/fft.cpp
    src/dsp/limiter.cpp
    src/dsp/partitioned_convolver.cpp
    src/dsp/resampler.cpp
    src/dsp/reverb.cpp
    src/spatial/air_absorption.cpp
    src/spatial/spatial_chain.cpp
    src/spatial/voice_jit.cpp
    src/spatial/voice_jit_builder.cpp
    src/synth/oscillator.cpp
    src/synth/modulation.cpp
    src/synth/voice.cpp
    src/synth/wavetable.cpp
    src/synth/voice_allocator.cpp
    src/synth/synth_parse.cpp
    src/synth/synth_voice.cpp
    src/synth/synth_loop.cpp
    src/synth/synth_interp.cpp
    src/synth/synth_jit.cpp
    src/clip/clip.cpp
    src/clip/file_stream.cpp
    src/sequencer/automation.cpp
    src/sequencer/sequence.cpp
    src/io/audio_file.cpp
    src/io/audio_stream.cpp
    src/io/byte_source.cpp
    src/io/serialization.cpp
    src/io/tags/audio_tags.cpp
    src/io/tags/id3.cpp
    src/io/tags/mp3.cpp
    src/io/tags/mp4.cpp
    src/io/tags/ogg.cpp
    src/io/tags/vorbis.cpp
    src/io/tags/wav.cpp
    src/codec/aac_decoder.cpp
    src/codec/mp4_aac.cpp
    src/engine.cpp
    src/engine_buses.cpp
    src/engine_bus_params.cpp
    src/engine_voices.cpp
    src/engine_capture.cpp
    src/engine_clips.cpp
    src/engine_presets.cpp
    src/engine_bus_fx.cpp
    src/engine_distance.cpp
    src/engine_playback_mix.cpp
    src/engine_synth.cpp
    src/log.cpp
    src/mic_tap.cpp
    src/device/device.cpp
    src/device/backend_sdl.cpp
    src/device/backend_null.cpp
    src/device/stream_resampler.cpp
    src/rcu.cpp
    src/dsp/jit/jit_bus_builder.cpp
    src/dsp/jit/jit_bus_pipeline.cpp
    src/dsp/jit/jit_compiler.cpp
    src/ear/ear_signal.cpp
    src/ear/ear_partials.cpp
    src/ear/ear_measure.cpp
    src/ear/ear_compare.cpp
    src/ear/ear_spectrogram.cpp
    src/ear/ear_font.cpp
    src/ear/ear_png.cpp
    src/ear/ear_cmaes.cpp
    src/ear/ear_fit.cpp
    src/ear/ear_fit_objective.cpp
)

# Loopback / process-audio capture: Windows uses WASAPI, macOS uses CoreAudio
# process taps, Linux uses PulseAudio (when libpulse is available; this also
# covers pipewire-pulse), everything else gets a stub.
if(WIN32)
    target_sources(broaudio PRIVATE src/loopback/capture_win.cpp)
    # ole32: COM; mmdevapi: ActivateAudioInterfaceAsync; uuid: GUIDs.
    target_link_libraries(broaudio PUBLIC ole32 mmdevapi uuid)
elseif(APPLE)
    # Objective-C++: CATapDescription is an ObjC class. Process taps + aggregate
    # device + HAL IO all live in CoreAudio; Foundation for the ObjC runtime.
    enable_language(OBJCXX)
    target_sources(broaudio PRIVATE src/loopback/capture_macos.mm)
    set_source_files_properties(src/loopback/capture_macos.mm PROPERTIES
        COMPILE_OPTIONS "-std=gnu++20")
    target_link_libraries(broaudio PUBLIC "-framework CoreAudio" "-framework Foundation")
    message(STATUS "broaudio: loopback capture via CoreAudio process taps")
else()
    set(BROAUDIO_LOOPBACK_BACKEND "stub")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBPULSE IMPORTED_TARGET libpulse)
    endif()
    if(LIBPULSE_FOUND)
        set(BROAUDIO_LOOPBACK_BACKEND "pulse")
    endif()

    if(BROAUDIO_LOOPBACK_BACKEND STREQUAL "pulse")
        target_sources(broaudio PRIVATE src/loopback/capture_pulse.cpp)
        target_link_libraries(broaudio PRIVATE PkgConfig::LIBPULSE)
        message(STATUS "broaudio: loopback capture via PulseAudio")
    else()
        target_sources(broaudio PRIVATE src/loopback/capture_null.cpp)
        message(STATUS "broaudio: loopback capture unsupported (no backend), using stub")
    endif()
endif()

# AAC (MP4/M4A audio) through the platform's decoder: Media Foundation on
# Windows, AudioToolbox on macOS. broaudio carries no AAC decoder of its own,
# so elsewhere M4A fails with "M4A/AAC needs a platform AAC decoder".
if(WIN32)
    target_sources(broaudio PRIVATE src/codec/aac_mediafoundation.cpp)
    target_link_libraries(broaudio PUBLIC mfplat mfuuid)
    target_compile_definitions(broaudio PUBLIC BROAUDIO_HAS_AAC=1)
    message(STATUS "broaudio: M4A/AAC via Media Foundation")
elseif(APPLE)
    target_sources(broaudio PRIVATE src/codec/aac_audiotoolbox.cpp)
    target_link_libraries(broaudio PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
    target_compile_definitions(broaudio PUBLIC BROAUDIO_HAS_AAC=1)
    message(STATUS "broaudio: M4A/AAC via AudioToolbox")
else()
    target_compile_definitions(broaudio PUBLIC BROAUDIO_HAS_AAC=0)
    message(STATUS "broaudio: no platform AAC decoder; M4A/AAC disabled")
endif()

target_sources(broaudio PRIVATE src/midi/midi_input.cpp)
if(BROAUDIO_MIDI)
    target_compile_definitions(broaudio PUBLIC BROAUDIO_HAS_MIDI=1)
endif()

# Use target properties instead of directory-scoped variables so this
# works correctly both standalone and when included via add_subdirectory.
target_compile_features(broaudio PUBLIC cxx_std_20)
target_include_directories(broaudio PUBLIC include)
target_include_directories(broaudio PRIVATE
    third_party/dr_libs
    third_party)

if(MSVC)
    target_compile_options(broaudio PRIVATE /MP /permissive- /utf-8)
    # Work around MSVC ICE in Release with /O2 on engine.cpp. The mic-capture
    # code split out into engine_capture.cpp still trips it at /O2.
    set_source_files_properties(src/engine.cpp src/engine_capture.cpp PROPERTIES
        COMPILE_FLAGS "$<$<NOT:$<CONFIG:Debug>>:/O1>")
endif()

# SDL3 is private: broaudio's public headers name no SDL type (device I/O is
# behind include/broaudio/device.h), so SDL's include directory does not reach
# consumers. A static broaudio still carries SDL onto their link line.
if(TARGET SDL3::SDL3)
    target_link_libraries(broaudio PRIVATE SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(broaudio PRIVATE SDL3::SDL3-static)
endif()

# Native PipeWire device backend (Linux): the default there when a daemon is
# reachable, with SDL as the fallback. Needs libpipewire-0.3 at build time;
# without it the build degrades to SDL only.
option(BROAUDIO_PIPEWIRE "Native PipeWire device backend on Linux" ON)
set(BROAUDIO_HAS_PIPEWIRE OFF)
if(BROAUDIO_PIPEWIRE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(BROAUDIO_LIBPIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    endif()
    if(BROAUDIO_LIBPIPEWIRE_FOUND)
        set(BROAUDIO_HAS_PIPEWIRE ON)
        target_sources(broaudio PRIVATE src/device/backend_pipewire.cpp)
        target_link_libraries(broaudio PRIVATE PkgConfig::BROAUDIO_LIBPIPEWIRE)
        message(STATUS "broaudio: PipeWire device backend (libpipewire ${BROAUDIO_LIBPIPEWIRE_VERSION})")
    else()
        message(STATUS "broaudio: libpipewire-0.3 not found, PipeWire device backend disabled (SDL only)")
    endif()
endif()
target_compile_definitions(broaudio PRIVATE BROAUDIO_HAS_PIPEWIRE=$<BOOL:${BROAUDIO_HAS_PIPEWIRE}>)

target_link_libraries(broaudio PUBLIC bromath::bromath)

# --- Brass JIT acceleration (brass arrives with bronze, above) ---
if(TARGET brass AND NOT BRASS_HOST_BACKEND)
    message(STATUS "broaudio: brass has no code generator for this target; JIT off")
elseif(TARGET brass)
    message(STATUS "broaudio: found Brass target")
    target_link_libraries(broaudio PUBLIC brass)
    target_compile_definitions(broaudio PUBLIC BROAUDIO_HAS_JIT=1)
    set(BROAUDIO_HAS_JIT 1)
    if(DEFINED BRASS_ROOT AND EXISTS "${BRASS_ROOT}/include")
        target_include_directories(broaudio PUBLIC "${BRASS_ROOT}/include")
    endif()
else()
    message(STATUS "broaudio: Brass not found; JIT disabled")
endif()

# Vendored codec implementations (see broaudio_codecs above). PUBLIC so the
# decoder symbols reach the final link line of every consumer of broaudio.
target_link_libraries(broaudio PUBLIC broaudio_codecs)

# The lock-free atomic<shared_ptr> RCU types lower some operations (e.g.
# is_lock_free) to libatomic calls. gcc links libatomic implicitly, but clang
# on Linux does not, so the link fails with undefined __atomic_* references.
# Link it by name so the compiler driver's -latomic resolves it from gcc's own
# library path (find_library does not search there). PUBLIC so the test
# executables, which reference the same intrinsics, pick it up too.
if(UNIX AND NOT APPLE)
    target_link_libraries(broaudio PUBLIC atomic)
endif()

if(BROAUDIO_MIDI AND TARGET libremidi::libremidi)
    target_link_libraries(broaudio PUBLIC libremidi::libremidi)
endif()

if(BROAUDIO_HAS_LIBOPUS)
    target_sources(broaudio PRIVATE src/codec/ogg_opus.cpp)
    target_compile_definitions(broaudio PUBLIC BROAUDIO_HAS_OPUS=1)
    target_link_libraries(broaudio PRIVATE Opus::opus)
endif()

# Bronze JavaScript API (before tests, which add a test against it)
add_subdirectory(src/api)

# Tests
option(BROAUDIO_TESTS "Build broaudio tests" ON)
if(BROAUDIO_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()


