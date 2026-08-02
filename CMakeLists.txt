cmake_minimum_required(VERSION 3.24)

project(mpxadrv VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(MDXMINI_INCLUDE_DIR
  NAMES libmdxmini/mdxmini.h
  HINTS
    /opt/homebrew/opt/mdxmini/include
    /usr/local/opt/mdxmini/include
)

find_library(MDXMINI_LIBRARY
  NAMES mdxmini
  HINTS
    /opt/homebrew/opt/mdxmini/lib
    /usr/local/opt/mdxmini/lib
)

if(NOT MDXMINI_INCLUDE_DIR OR NOT MDXMINI_LIBRARY)
  message(FATAL_ERROR
    "mdxmini was not found. Install it with: brew install mdxmini")
endif()

find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)
find_library(ICONV_LIBRARY iconv REQUIRED)

add_executable(mpxadrv
  src/compat.cpp
  src/core_midi.cpp
  src/main.cpp
  src/mdr.cpp
  src/midi.cpp
  src/software_synth.cpp
  src/tdx.cpp
)

target_include_directories(mpxadrv PRIVATE "${MDXMINI_INCLUDE_DIR}")
target_link_libraries(mpxadrv PRIVATE
  "${MDXMINI_LIBRARY}"
  "${AUDIOTOOLBOX_FRAMEWORK}"
  "${COREFOUNDATION_FRAMEWORK}"
  "${COREMIDI_FRAMEWORK}"
  "${ICONV_LIBRARY}"
)

target_compile_options(mpxadrv PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME help COMMAND mpxadrv --help)
  add_test(NAME version COMMAND mpxadrv --version)
  set_tests_properties(version PROPERTIES
    PASS_REGULAR_EXPRESSION "macOS CLI adaptation developed by Awed"
  )

  add_executable(tdx_tests
    tests/tdx_tests.cpp
    src/tdx.cpp
  )
  target_include_directories(tdx_tests PRIVATE src)
  target_link_libraries(tdx_tests PRIVATE "${ICONV_LIBRARY}")
  target_compile_options(tdx_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME tdx_compiler COMMAND tdx_tests)

  add_executable(compat_tests
    tests/compat_tests.cpp
    src/compat.cpp
  )
  target_include_directories(compat_tests PRIVATE src)
  target_compile_options(compat_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME compatibility_scanner COMMAND compat_tests)

  add_executable(mdr_tests
    tests/mdr_tests.cpp
    src/mdr.cpp
  )
  target_include_directories(mdr_tests PRIVATE src)
  target_compile_options(mdr_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME mdr_loader COMMAND mdr_tests)

  add_executable(midi_tests
    tests/midi_tests.cpp
    src/midi.cpp
  )
  target_include_directories(midi_tests PRIVATE src)
  target_compile_options(midi_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME midi_conversion COMMAND midi_tests)
endif()

install(TARGETS mpxadrv RUNTIME DESTINATION bin)
