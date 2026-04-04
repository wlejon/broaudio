cmake_minimum_required(VERSION 3.24)
project(broaudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/MP /permissive- /utf-8)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# SDL3 — use consumer-provided target if available, otherwise find it
if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
    find_package(SDL3 REQUIRED)
endif()

add_library(broaudio STATIC
    src/core/ring_buffer.cpp
    src/dsp/biquad.cpp
    src/dsp/compressor.cpp
    src/dsp/delay.cpp
    src/dsp/equalizer.cpp
    src/dsp/fft.cpp
    src/dsp/limiter.cpp
    src/synth/oscillator.cpp
    src/synth/voice.cpp
    src/clip/clip.cpp
    src/engine.cpp
)

target_include_directories(broaudio PUBLIC include)

if(TARGET SDL3::SDL3)
    target_link_libraries(broaudio PUBLIC SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(broaudio PUBLIC SDL3::SDL3-static)
endif()
