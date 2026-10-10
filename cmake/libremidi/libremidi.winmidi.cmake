# broaudio's replacement for libremidi's cmake/libremidi.winmidi.cmake (pinned
# celtera/libremidi 6e8dccd): the same Windows MIDI Services setup, but the
# Midi2 projection headers in ${CMAKE_BINARY_DIR}/cppwinrt-winmidi are
# generated idempotently (libremidi.winrt_generate.cmake), and the NuGet
# package is extracted only when it is newer than what was extracted, instead
# of both being redone on every configure.
if(LIBREMIDI_NO_WINMIDI)
  return()
endif()

include(libremidi.winrt_generate)

if(LIBREMIDI_DOWNLOAD_CPPWINRT)
  if(NOT EXISTS "${CMAKE_BINARY_DIR}/winmidi-headers.zip")
    file(DOWNLOAD
      https://github.com/microsoft/MIDI/releases/download/rc-2/Microsoft.Windows.Devices.Midi2.1.0.15-rc.2.15.nupkg
      "${CMAKE_BINARY_DIR}/winmidi-headers.zip"
    )
  endif()
  set(LIBREMIDI_WINMIDI_HEADERS_ZIP "${CMAKE_BINARY_DIR}/winmidi-headers.zip")
endif()

if(NOT LIBREMIDI_WINMIDI_HEADERS_ZIP)
  return()
endif()

set(_winmidi_winmd "${CMAKE_BINARY_DIR}/winmidi-headers/ref/native/Microsoft.Windows.Devices.Midi2.winmd")
# Extracted files keep the archive's dates, so a stamp written at extraction
# says whether the package changed since.
set(_winmidi_extracted "${CMAKE_BINARY_DIR}/winmidi-headers.extracted")
if(NOT EXISTS "${_winmidi_winmd}" OR NOT EXISTS "${_winmidi_extracted}"
   OR "${LIBREMIDI_WINMIDI_HEADERS_ZIP}" IS_NEWER_THAN "${_winmidi_extracted}")
  file(ARCHIVE_EXTRACT
    INPUT "${LIBREMIDI_WINMIDI_HEADERS_ZIP}"
    DESTINATION "${CMAKE_BINARY_DIR}/winmidi-headers/"
  )
  file(TOUCH "${_winmidi_extracted}")
endif()

file(MAKE_DIRECTORY
  "${CMAKE_BINARY_DIR}/cppwinrt/"
)

if(CPPWINRT_TOOL)
  # Enumerate winmd IDL files and store them in a response file
  file(TO_CMAKE_PATH "${CMAKE_WINDOWS_KITS_10_DIR}/References/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}" winsdk)
  file(GLOB winmds "${winsdk}/*/*/*.winmd")
  list(SORT winmds)

  set(args "")
  string(APPEND args "-input \"${_winmidi_winmd}\"\n")

  foreach(winmd IN LISTS winmds)
    string(APPEND args "-ref \"${winmd}\"\n")
  endforeach()

  _libremidi_winrt_generate("${CMAKE_BINARY_DIR}/cppwinrt-winmidi" cppwinrt-winmidi.rsp "${args}" "${CPPWINRT_TOOL}"
    INPUTS
      "${_winmidi_winmd}"
    EXTRA
      "${CMAKE_BINARY_DIR}/winmidi-headers/build/native/include/winmidi/init"
      "${CMAKE_BINARY_DIR}/winmidi-headers/build/native/include/winmidi/WindowsMidiServicesAppSdkComExtensions.h"
  )
else()
  # In case we don't have cppwinrt we can still try to just use the SDK headers directly
  file(
    COPY
      "${CMAKE_BINARY_DIR}/winmidi-headers/build/native/include/winmidi"
    DESTINATION
      "${CMAKE_BINARY_DIR}/cppwinrt-winmidi/"
  )
endif()

message(STATUS "libremidi: using Windows MIDI Services")
set(LIBREMIDI_HAS_WINMIDI 1)

target_include_directories(libremidi SYSTEM ${_public}
  $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/cppwinrt>
  $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/cppwinrt-winmidi>
  $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/cppwinrt-winmidi/winmidi>
)
target_compile_definitions(libremidi ${_public} LIBREMIDI_WINMIDI)
target_link_libraries(libremidi ${_public} RuntimeObject windowsapp)
