cmake_minimum_required(VERSION 3.24)
project(broaudio LANGUAGES CXX)

# When built standalone, set the MSVC runtime to static
if(MSVC AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# SDL3 — use consumer-provided target if available, otherwise find it
if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
    find_package(SDL3 REQUIRED)
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

add_library(broaudio STATIC
    src/core/ring_buffer.cpp
    src/dsp/biquad.cpp
    src/dsp/chorus.cpp
    src/dsp/compressor.cpp
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
    src/sequencer/automation.cpp
    src/sequencer/sequence.cpp
    src/io/audio_file.cpp
    src/io/serialization.cpp
    src/engine.cpp
)

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
