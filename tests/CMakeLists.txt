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
    test_chorus
    test_ring_buffer
    test_voice_allocator
    test_effect_order
    test_unison
    test_sequence
    test_bus_eq
    test_bus_metering
    test_sidechain
    test_scheduled_events
)

foreach(test ${BROAUDIO_TESTS})
    add_executable(${test} ${test}.cpp)
    target_link_libraries(${test} PRIVATE broaudio)
    target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${test} COMMAND ${test})
endforeach()
