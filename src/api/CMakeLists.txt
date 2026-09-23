cmake_minimum_required(VERSION 3.24)

set(BROAUDIO_API_SOURCES
    host_class.cpp
    api.cpp
    host_audio_context.cpp
    host_audio_nodes.cpp
    host_audio_param.cpp
    host_audio_live.cpp
    host_audio_spatial.cpp
    host_audio_buffer.cpp
    host_audio_dsp.cpp
    host_audio_synth.cpp
    host_audio_clips.cpp
    host_audio_playback.cpp
    host_audio_voice_ext.cpp
    host_audio_bus_fx.cpp
    host_audio_synth_ext.cpp
    host_audio_sequence_ext.cpp
    host_audio_presets.cpp
    host_audio_io.cpp
    native_mic.cpp
)

add_library(broaudio_api STATIC
    ${BROAUDIO_API_SOURCES}
)

target_include_directories(broaudio_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(broaudio_api PUBLIC
    broaudio
    bronze_runtime_shared
)

target_compile_features(broaudio_api PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(broaudio_api PRIVATE /MP /permissive- /utf-8)
endif()
