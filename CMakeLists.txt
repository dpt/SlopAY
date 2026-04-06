cmake_minimum_required(VERSION 4.2)
project(SlopAY VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(CTest)

set(SLOPAY_STRICT_WARNING_FLAGS)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set(SLOPAY_STRICT_WARNING_FLAGS -Wall -Wextra -pedantic)
endif()

add_executable(SlopAY
  lib/slopay-chip.c
  slop-ay/slopay-loader.c
  slop-ay/slopay-target-midi.c
  slop-ay/slopay-target-wave.c
  slop-ay/slopay.c
  slop-ay/slopz80.c
)

target_include_directories(SlopAY PRIVATE
  "${CMAKE_SOURCE_DIR}/lib"
)

target_compile_definitions(SlopAY PRIVATE
  SLOPAY_VERSION_STRING="${PROJECT_VERSION}"
)

target_compile_options(SlopAY PRIVATE ${SLOPAY_STRICT_WARNING_FLAGS})

if(APPLE)
  find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
  find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)

  target_sources(SlopAY PRIVATE
    lib/slopay-target-macos.c
  )

  target_compile_definitions(SlopAY PRIVATE
    SLOPAY_HAVE_MACOS_AUDIO=1
  )

  target_link_libraries(SlopAY PRIVATE
    ${COREAUDIO_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
  )

  add_executable(MIDIAY
    lib/slopay-chip.c
    lib/slopay-target-macos.c
    midi-ay/chords.c
    midi-ay/effects/echo.c
    midi-ay/effects/reverb.c
    midi-ay/midiay.c
    midi-ay/polyblep.c
  )

  target_include_directories(MIDIAY PRIVATE
    "${CMAKE_SOURCE_DIR}/lib"
  )

  target_compile_definitions(MIDIAY PRIVATE
    SLOPAY_HAVE_MACOS_AUDIO=1
    SLOPAY_VERSION_STRING="${PROJECT_VERSION}"
  )

  target_compile_options(MIDIAY PRIVATE ${SLOPAY_STRICT_WARNING_FLAGS})

  target_link_libraries(MIDIAY PRIVATE
    ${COREAUDIO_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
    ${COREMIDI_FRAMEWORK}
  )
endif()

if(BUILD_TESTING)
  add_executable(slopay-chip-test
    tests/slopay-chip-test.c
    lib/slopay-chip.c
  )

  add_executable(slopay-loader-test
    tests/slopay-loader-test.c
    slop-ay/slopay-loader.c
  )

  target_include_directories(slopay-chip-test PRIVATE
    "${CMAKE_SOURCE_DIR}/lib"
  )

  target_include_directories(slopay-loader-test PRIVATE
    "${CMAKE_SOURCE_DIR}/slop-ay"
  )

  target_compile_options(slopay-chip-test PRIVATE ${SLOPAY_STRICT_WARNING_FLAGS})
  target_compile_options(slopay-loader-test PRIVATE ${SLOPAY_STRICT_WARNING_FLAGS})

  add_test(NAME slopay-chip-test COMMAND slopay-chip-test)
  add_test(NAME slopay-loader-test COMMAND slopay-loader-test)

  add_test(NAME slopay-version COMMAND $<TARGET_FILE:SlopAY> --version)
  set_tests_properties(slopay-version PROPERTIES
    PASS_REGULAR_EXPRESSION "^SlopAY [0-9]+\\.[0-9]+\\.[0-9]+"
  )

  add_test(NAME slopay-help COMMAND $<TARGET_FILE:SlopAY> --help)
  set_tests_properties(slopay-help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: .*SlopAY"
  )
endif()
