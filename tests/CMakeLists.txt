enable_testing()

# Each test is a separate executable that links against broaudio.
# The test harness is header-only (test_harness.h).

set(BROAUDIO_TESTS
    test_oscillator
    test_biquad
    test_fft
    test_spatial
    test_modulation
    test_wavetable
    test_compressor
    test_limiter
    test_delay
    test_reverb
    test_convolution_reverb
    test_chorus
    test_distortion
    test_ring_buffer
    test_voice_allocator
    test_effect_order
    test_unison
    test_automation
    test_sequence
    test_bus_eq
    test_bus_metering
    test_sidechain
    test_scheduled_events
    test_signal_chain
    test_smoothing
    test_resampler
    test_audio_file
    test_vorbis
    test_file_stream
    test_serialization
    test_atomic_shared_ptr
    test_clip_playback
    test_engine_spatial
    test_engine_routing
    test_engine_presets
    test_engine_offline
    test_mic_tap
    test_loopback_capture
    test_midi_input
)

foreach(test ${BROAUDIO_TESTS})
    add_executable(${test} ${test}.cpp)
    target_link_libraries(${test} PRIVATE broaudio)
    target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${test} COMMAND ${test})
endforeach()

# JIT audio bus pipeline test (requires Brass JIT backend)
if(BROAUDIO_HAS_JIT)
    add_executable(test_jit_pipeline test_jit_pipeline.cpp)
    target_link_libraries(test_jit_pipeline PRIVATE broaudio)
    target_include_directories(test_jit_pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME test_jit_pipeline COMMAND test_jit_pipeline)
endif()

# Bronze JavaScript binding (broaudio_api): Web Audio mount points, an
# AudioContext node graph, createBus, TypeErrors on bad arguments. Never
# resumes the context or starts playback.
if(TARGET broaudio_api)
    add_executable(broaudio_test_api test_audio_api.cpp)
    target_link_libraries(broaudio_test_api PRIVATE broaudio_api bronze_runtime_shared)
    # bronze's shared runtime is built into BRONZE_SHARED_RUNTIME_DIR, not
    # beside this executable, and the PE loader only looks beside the .exe
    # (0xc0000135 + a modal "dll was not found" otherwise). Stage it next to
    # the test as a post-build step and put both dirs on ctest's PATH.
    if(WIN32)
        add_custom_command(TARGET broaudio_test_api POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broaudio_test_api>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broaudio_test_api>
        )
    endif()
    # It runs JavaScript, which brass compiles to machine code for the target;
    # on one brass has no code generator for (BRASS_HOST_BACKEND, decided in
    # brass's CMakeLists — Apple Silicon today) that code cannot run. The
    # executable still builds there, which is the check that the embed links;
    # the test is not registered rather than registered to bus-error.
    if(NOT BRASS_HOST_BACKEND)
        message(STATUS "broaudio: broaudio_test_api not registered (brass has no code generator for this target)")
    else()
        add_test(NAME broaudio_test_api COMMAND broaudio_test_api)
        if(WIN32)
            set_tests_properties(broaudio_test_api PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broaudio_test_api>;$ENV{PATH}")
        endif()
    endif()

    # Pre-bronze binding semantics (drift rows B1-B17 of bro's
    # docs/transition-drift.md plus the node-level shape diff): its own
    # headless engine, renderBlock-driven, files through a scratch resolver.
    add_executable(broaudio_test_api_drift test_audio_api_drift.cpp)
    target_link_libraries(broaudio_test_api_drift PRIVATE broaudio_api bronze_runtime_shared)
    if(WIN32)
        add_custom_command(TARGET broaudio_test_api_drift POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broaudio_test_api_drift>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broaudio_test_api_drift>
        )
    endif()
    if(BRASS_HOST_BACKEND)
        add_test(NAME broaudio_test_api_drift COMMAND broaudio_test_api_drift)
        if(WIN32)
            set_tests_properties(broaudio_test_api_drift PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broaudio_test_api_drift>;$ENV{PATH}")
        endif()
    endif()

    # ctx.destination is a real AudioDestinationNode over AudioNode.prototype.
    add_executable(broaudio_test_destination test_audio_destination.cpp)
    target_link_libraries(broaudio_test_destination PRIVATE broaudio_api bronze_runtime_shared)
    if(WIN32)
        add_custom_command(TARGET broaudio_test_destination POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broaudio_test_destination>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broaudio_test_destination>
        )
    endif()
    if(BRASS_HOST_BACKEND)
        add_test(NAME broaudio_test_destination COMMAND broaudio_test_destination)
        if(WIN32)
            set_tests_properties(broaudio_test_destination PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broaudio_test_destination>;$ENV{PATH}")
        endif()
    endif()

    # The binding against embed.h's GC contract: paths that hold a Value
    # across an allocating call, plus the fixes that came with that audit.
    add_executable(broaudio_test_api_gc test_audio_api_gc.cpp)
    target_link_libraries(broaudio_test_api_gc PRIVATE broaudio_api bronze_runtime_shared)
    if(WIN32)
        add_custom_command(TARGET broaudio_test_api_gc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broaudio_test_api_gc>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:broaudio_test_api_gc>
        )
    endif()
    if(BRASS_HOST_BACKEND)
        add_test(NAME broaudio_test_api_gc COMMAND broaudio_test_api_gc)
        if(WIN32)
            set_tests_properties(broaudio_test_api_gc PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:broaudio_test_api_gc>;$ENV{PATH}")
        endif()

        # Every binding test again under bronze's GC stress oracle: a
        # collection on every allocation, from-space poisoned afterwards, so a
        # Value held across an allocating embed call crashes or reads garbage
        # here instead of passing by luck.
        foreach(api_test broaudio_test_api broaudio_test_api_drift
                         broaudio_test_destination broaudio_test_api_gc)
            add_test(NAME ${api_test}_gcstress COMMAND ${api_test})
            if(WIN32)
                set_tests_properties(${api_test}_gcstress PROPERTIES
                    ENVIRONMENT "BRONZE_GC_STRESS=1;BRONZE_GC_POISON=1;PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:${api_test}>;$ENV{PATH}")
            else()
                set_tests_properties(${api_test}_gcstress PROPERTIES
                    ENVIRONMENT "BRONZE_GC_STRESS=1;BRONZE_GC_POISON=1")
            endif()
        endforeach()
    endif()
endif()
