cmake_minimum_required(VERSION 4.2)
project(SlopAY C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(SlopAY
  slop-ay/slopay.c
  slop-ay/slopay-loader.c
  slop-ay/slopay-z80.c
  lib/slopay-chip.c
  slop-ay/slopay-target-wave.c
)

target_include_directories(SlopAY PRIVATE
  "${CMAKE_SOURCE_DIR}/lib"
)

if(APPLE)
  find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
  find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)

  target_sources(SlopAY PRIVATE
    lib/slopay-target-macos.c
  )

  target_link_libraries(SlopAY PRIVATE
    ${COREAUDIO_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
  )

  add_executable(MIDIAY
    midi-ay/midiay.c
    lib/slopay-chip.c
    lib/slopay-target-macos.c
    midi-ay/polyblep.c
    midi-ay/reverb.c
  )

  target_include_directories(MIDIAY PRIVATE
    "${CMAKE_SOURCE_DIR}/lib"
  )

  target_link_libraries(MIDIAY PRIVATE
    ${COREAUDIO_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
    ${COREMIDI_FRAMEWORK}
  )
endif()
