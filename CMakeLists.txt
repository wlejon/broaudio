cmake_minimum_required(VERSION 3.24)
# C is required for the vendored stb_vorbis translation unit
# (src/codec/stb_vorbis_impl.c does not compile as C++).
project(broaudio LANGUAGES CXX C)

# When built standalone, set the MSVC runtime to static
if(MSVC AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# SDL3 — use consumer-provided target if available, otherwise find it
if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
    find_package(SDL3 REQUIRED)
endif()

# bromath — header-only math library (sibling repo)
set(BROMATH_DIR "${CMAKE_SOURCE_DIR}/../bromath" CACHE PATH "bromath sibling")
if(NOT TARGET bromath::bromath)
    if(EXISTS "${BROMATH_DIR}/CMakeLists.txt")
        add_subdirectory("${BROMATH_DIR}" "${CMAKE_BINARY_DIR}/bromath" EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "bromath not found at ${BROMATH_DIR}")
    endif()
endif()

# opusfile — optional OGG Opus support
option(BROAUDIO_OPUS "Enable OGG Opus loading via opusfile" OFF)
if(BROAUDIO_OPUS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(OPUSFILE IMPORTED_TARGET opusfile)
    endif()
    if(NOT OPUSFILE_FOUND)
        find_package(OpusFile QUIET)
    endif()
    if(NOT OPUSFILE_FOUND AND NOT TARGET OpusFile::opusfile)
        set(BROAUDIO_OPUS OFF)
        message(STATUS "broaudio: opusfile not found, OGG Opus support disabled")
    endif()
endif()

# libremidi — optional MIDI support
option(BROAUDIO_MIDI "Enable MIDI input via libremidi" ON)
if(BROAUDIO_MIDI)
    if(NOT TARGET libremidi::libremidi)
        # Build from submodule if available
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libremidi/CMakeLists.txt")
            set(LIBREMIDI_NO_JACK ON CACHE BOOL "" FORCE)
            set(LIBREMIDI_NO_PIPEWIRE ON CACHE BOOL "" FORCE)
            set(LIBREMIDI_NO_NETWORK ON CACHE BOOL "" FORCE)
            set(LIBREMIDI_NO_KEYBOARD ON CACHE BOOL "" FORCE)
            add_subdirectory(third_party/libremidi EXCLUDE_FROM_ALL)
        else()
            find_package(libremidi QUIET)
        endif()
    endif()
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
    src/dsp/resampler.cpp
    src/dsp/reverb.cpp
    src/synth/oscillator.cpp
    src/synth/modulation.cpp
    src/synth/voice.cpp
    src/synth/wavetable.cpp
    src/synth/voice_allocator.cpp
    src/clip/clip.cpp
    src/clip/file_stream.cpp
    src/sequencer/automation.cpp
    src/sequencer/sequence.cpp
    src/io/audio_file.cpp
    src/io/audio_stream.cpp
    src/io/serialization.cpp
    src/engine.cpp
    src/log.cpp
    src/mic_tap.cpp
    src/rcu.cpp
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

if(BROAUDIO_MIDI)
    target_sources(broaudio PRIVATE src/midi/midi_input.cpp)
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
    # Work around MSVC ICE in Release with /O2 on engine.cpp
    set_source_files_properties(src/engine.cpp PROPERTIES
        COMPILE_FLAGS "$<$<NOT:$<CONFIG:Debug>>:/O1>")
endif()

if(TARGET SDL3::SDL3)
    target_link_libraries(broaudio PUBLIC SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(broaudio PUBLIC SDL3::SDL3-static)
endif()

target_link_libraries(broaudio PUBLIC bromath::bromath)

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

if(BROAUDIO_OPUS)
    target_compile_definitions(broaudio PUBLIC BROAUDIO_HAS_OPUS=1)
    if(TARGET PkgConfig::OPUSFILE)
        target_link_libraries(broaudio PRIVATE PkgConfig::OPUSFILE)
    elseif(TARGET OpusFile::opusfile)
        target_link_libraries(broaudio PRIVATE OpusFile::opusfile)
    endif()
endif()

# Tests
option(BROAUDIO_TESTS "Build broaudio tests" ON)
if(BROAUDIO_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()
