# Idempotent C++/WinRT header generation for libremidi's Windows backends.
#
# Upstream libremidi (cmake/libremidi.cppwinrt.cmake, libremidi.winmidi.cmake)
# deletes ${CMAKE_BINARY_DIR}/cppwinrt*/ and regenerates every header on each
# configure, so a reconfigure breaks any build compiling against them at that
# moment ("winrt/... .h: No such file", "Mismatched C++/WinRT headers").
# broaudio puts this directory ahead of libremidi's own on CMAKE_MODULE_PATH,
# so its include(libremidi.cppwinrt) / include(libremidi.winmidi) find the
# versions here, which generate through this helper:
#
#   - nothing runs when the output exists and its inputs (the cppwinrt tool,
#     the SDK's .winmd list, the extra files) are what the last run used;
#   - otherwise the tool writes into a temporary directory, and only files
#     whose bytes changed are copied over the live ones; nothing is removed,
#     so a header a running build is reading never disappears.

include_guard(GLOBAL)

# _libremidi_winrt_generate(<out dir> <rsp name> <rsp text> <tool> [INPUTS <file>...] [EXTRA <file|dir>...])
# INPUTS are further files whose change means regenerating; EXTRA paths are
# copied into the output beside the generated headers (and count as inputs).
function(_libremidi_winrt_generate out rsp args tool)
  cmake_parse_arguments(PARSE_ARGV 4 G "" "" "INPUTS;EXTRA")
  set(key "${tool}|${args}")
  foreach(input IN ITEMS "${tool}" ${G_INPUTS} ${G_EXTRA})
    file(TIMESTAMP "${input}" ts "%Y%m%d%H%M%S" UTC)
    string(APPEND key "|${input}@${ts}")
  endforeach()
  string(SHA256 key "${key}")

  set(stamp "${out}.stamp")
  if(IS_DIRECTORY "${out}" AND EXISTS "${stamp}")
    file(READ "${stamp}" previous)
    if(previous STREQUAL key)
      return()
    endif()
  endif()

  set(tmp "${out}.tmp")
  file(REMOVE_RECURSE "${tmp}")
  file(WRITE "${CMAKE_BINARY_DIR}/cppwinrt-src/${rsp}" "${args}")
  execute_process(
    COMMAND "${tool}" "@${CMAKE_BINARY_DIR}/cppwinrt-src/${rsp}" -output "${tmp}"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    file(REMOVE_RECURSE "${tmp}")
    message(WARNING "libremidi: cppwinrt failed (${rc}) for ${out}; keeping the existing headers")
    return()
  endif()
  if(G_EXTRA)
    file(COPY ${G_EXTRA} DESTINATION "${tmp}/")
  endif()

  # Swap in: write only what changed.
  file(GLOB_RECURSE generated RELATIVE "${tmp}" "${tmp}/*")
  foreach(rel IN LISTS generated)
    get_filename_component(dir "${out}/${rel}" DIRECTORY)
    file(MAKE_DIRECTORY "${dir}")
    file(COPY_FILE "${tmp}/${rel}" "${out}/${rel}" ONLY_IF_DIFFERENT)
  endforeach()
  file(REMOVE_RECURSE "${tmp}")
  file(WRITE "${stamp}" "${key}")
endfunction()
