# Linux build for the "Братья Пилоты" reconstruction.
# Windows builds keep using SRC/build_sdl_release.bat + build_sdl_debug.bat (untouched).
#
# 32-bit only (-m32): game_globals_struct.h stores pointers in 32-bit fields byte-for-byte
# like the original PILOTS.EXE data section - a native x86_64 build would corrupt the layout.
cmake_minimum_required(VERSION 3.16)
project(PILOT_RV C CXX)

# Debug logging on by default: DBG_* macros are our only diagnostics on Linux
# (writes PILOT_RV_debug.log next to the exe, backtrace on crash).
option(PILOTS_DEBUG_LOG "Define _DEBUG (debug log + crash handlers)" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# pkg-config must pick the i386 .pc files (multiarch), not the amd64 host ones
set(ENV{PKG_CONFIG_PATH}
    "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")

# -fno-strict-aliasing: the decompiled code type-puns through IDA macros (bmi/pwh overlays)
add_compile_options(-m32 -fno-strict-aliasing -fcommon)
add_link_options(-m32 -rdynamic)   # -rdynamic: symbol names in crash backtraces

if(PILOTS_DEBUG_LOG)
  add_compile_definitions(_DEBUG)
endif()

set(GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SRC)
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Same explicit list as build_sdl_release.bat (test_cast.c / test_wave.c stay out of the build)
set(GAME_SOURCES
  ${GAME_DIR}/globals.c
  ${GAME_DIR}/animation.c
  ${GAME_DIR}/render.c
  ${GAME_DIR}/gameloop.c
  ${GAME_DIR}/script.c
  ${GAME_DIR}/resource.c
  ${GAME_DIR}/saveload.c
  ${GAME_DIR}/audio.c
  ${GAME_DIR}/video.c
  ${GAME_DIR}/text.c
  ${GAME_DIR}/tilemap.c
  ${GAME_DIR}/blitting.c
  ${GAME_DIR}/graphics.c
  ${GAME_DIR}/main.c
  ${GAME_DIR}/fileio.c
  ${GAME_DIR}/memory.c
  ${GAME_DIR}/input.c
  ${GAME_DIR}/native_funcs.c
  ${GAME_DIR}/sdl_backend.c
  ${GAME_DIR}/win32_shim.c
  ${GAME_DIR}/mci_ffmpeg.c
)

add_executable(PILOT_RV ${GAME_SOURCES})
target_include_directories(PILOT_RV PRIVATE ${GAME_DIR})
# Fake <windows.h>/<mmsystem.h>/<io.h>/<vfw.h>/<intrin.h>/<crtdbg.h> -> win32_shim.h
target_include_directories(PILOT_RV PRIVATE ${GAME_DIR}/winshim)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)

target_link_libraries(PILOT_RV PRIVATE PkgConfig::SDL2 PkgConfig::AV m)

# ---- libADLMIDI: same source list and defines as the .bat builds ----
set(ADLMIDI_OPTS
  ADLMIDI_DISABLE_MIDI_SEQUENCER
  BWMIDI_DISABLE_XMI_SUPPORT
  ADLMIDI_DISABLE_OPAL_EMULATOR
  ADLMIDI_DISABLE_JAVA_EMULATOR
  ADLMIDI_DISABLE_ESFMU_EMULATOR
  ADLMIDI_DISABLE_MAME_OPL2_EMULATOR
  ADLMIDI_DISABLE_YMFM_EMULATOR)

set(ADLMIDI_CXX_SOURCES
  ${THIRD_PARTY}/libADLMIDI/src/adlmidi.cpp
  ${THIRD_PARTY}/libADLMIDI/src/adlmidi_load.cpp
  ${THIRD_PARTY}/libADLMIDI/src/adlmidi_midiplay.cpp
  ${THIRD_PARTY}/libADLMIDI/src/adlmidi_opl3.cpp
  ${THIRD_PARTY}/libADLMIDI/src/adlmidi_private.cpp
  ${THIRD_PARTY}/libADLMIDI/src/inst_db.cpp
  ${THIRD_PARTY}/libADLMIDI/src/chips/dosbox_opl2.cpp
  ${THIRD_PARTY}/libADLMIDI/src/chips/dosbox_opl3.cpp
  ${THIRD_PARTY}/libADLMIDI/src/chips/dosbox/dbopl.cpp
  ${THIRD_PARTY}/libADLMIDI/src/chips/nuked_opl3.cpp
  ${THIRD_PARTY}/libADLMIDI/src/chips/nuked_opl3_fast.cpp
  ${THIRD_PARTY}/libADLMIDI/src/chips/nuked_opl2.cpp
  ${THIRD_PARTY}/libADLMIDI/src/chips/nuked_cqm.cpp)

set(ADLMIDI_C_SOURCES
  ${THIRD_PARTY}/libADLMIDI/src/wopl/wopl_file.c
  ${THIRD_PARTY}/libADLMIDI/src/models/model_ail.c
  ${THIRD_PARTY}/libADLMIDI/src/models/model_apogee.c
  ${THIRD_PARTY}/libADLMIDI/src/models/model_dmx.c
  ${THIRD_PARTY}/libADLMIDI/src/models/model_generic.c
  ${THIRD_PARTY}/libADLMIDI/src/models/model_hmi_sos.c
  ${THIRD_PARTY}/libADLMIDI/src/models/model_msadlib.c
  ${THIRD_PARTY}/libADLMIDI/src/models/model_oconnell.c
  ${THIRD_PARTY}/libADLMIDI/src/models/model_win9x.c
  ${THIRD_PARTY}/libADLMIDI/src/chips/nuked/nukedopl3.c
  ${THIRD_PARTY}/libADLMIDI/src/chips/nuked_fast/nukedopl3_fast.c
  ${THIRD_PARTY}/libADLMIDI/src/chips/nuked/nukedopl2.c
  ${THIRD_PARTY}/libADLMIDI/src/chips/nuked_cqm/cqm.c)

add_library(adlmidi STATIC ${ADLMIDI_CXX_SOURCES} ${ADLMIDI_C_SOURCES})
target_include_directories(adlmidi PUBLIC ${THIRD_PARTY}/libADLMIDI/include)
target_compile_definitions(adlmidi PRIVATE ${ADLMIDI_OPTS})
target_compile_options(adlmidi PRIVATE -O2)   # the .bat builds libADLMIDI with /O2, game with /Od

target_link_libraries(PILOT_RV PRIVATE adlmidi)
