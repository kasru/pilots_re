# Linux + Nintendo Switch build for the "Братья Пилоты" reconstruction.
# Windows builds keep using SRC/build_sdl.bat [debug|release] [x86|x64] (untouched).
#
# Three flavours, all driven by the same CMakeLists:
#
#   cmake -S . -B build32 -DPILOTS_M32=ON    32-bit (-m32) - byte-comparable with
#                                             the original PILOTS.EXE data section
#   cmake -S . -B build64                    native (x86_64) - the LP64 migration
#                                             moved every decompiled pointer-width
#                                             field to INT_PTR, so the generated
#                                             headers lay out correctly on both
#   scripts/build_switch_podman.sh           Nintendo Switch - same LP64 layout,
#                                             cross-compiled by devkitA64 inside
#                                             the devkitpro/devkita64 container,
#                                             output PILOT_RV.nro
#
# Keeping them in separate build dirs matters for another reason: SDL2,
# libavcodec and friends must come from the matching architecture, so a build
# dir is tied to one of them for good.
cmake_minimum_required(VERSION 3.16)

# Must be decided before project(): the Switch toolchain file sets
# CMAKE_SYSTEM_NAME=NintendoSwitch, which is what selects Platform/*.cmake and
# brings nx_create_nro() into scope for the .nro step at the bottom.
option(PLATFORM_SWITCH "Cross-compile the Nintendo Switch .nro (devkitA64/libnx)" OFF)

project(PILOT_RV C CXX)

# Debug logging on by default: DBG_* macros are our only diagnostics on Linux
# (writes PILOT_RV_debug.log next to the exe, backtrace on crash). Off by default
# on the Switch, where the reason is app0:: a shipped NSO has its own filesystem
# mounted read-only, so "next to the exe" cannot be written.
#
# An NRO is the other case, and the one we actually run: libnx chdir's to the
# directory the .nro was loaded from (fs_dev.c __libnx_init_cwd, for anything
# that is not an NSO), and that directory is on sdmc. GetModuleFileNameA has no
# /proc/self/exe on newlib, so it answers the relative "PILOT_RV.EXE"
# (win32_shim.c), debug_log.h turns that into "PILOT_RV_debug.log" in the CWD -
# i.e. right next to the .nro and the game data in /switch/pilots. So the log is
# not only possible on the console, it is the only way to see anything there:
# scripts/build_switch_podman.sh debug turns it on.
if(PLATFORM_SWITCH)
  option(PILOTS_DEBUG_LOG "Define _DEBUG (debug log + crash handlers)" OFF)
else()
  option(PILOTS_DEBUG_LOG "Define _DEBUG (debug log + crash handlers)" ON)
endif()
option(PILOTS_M32 "Build 32-bit (-m32) instead of native" OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# -fno-strict-aliasing: the decompiled code type-puns through IDA macros (bmi/pwh overlays)
add_compile_options(-fno-strict-aliasing -fcommon)
# -fsigned-char: `char` in this code means the x86 `char`, which is SIGNED, and the
# original leaned on that default rather than saying it - LZSSDecompress (resource.c)
# ends its back-reference copy with
#     char v13 = (BYTE1(v10) & 0xF) + 2;  do { ...; --v13; } while ( v13 >= 0 );
# AArch64 (AAPCS64) makes plain `char` UNSIGNED by default, so on the console
# `v13 >= 0` is a tautology, the test folds away - the loop that shipped was
# literally `ldrb / strb [x1], #1 / b` with no counter left in it - and the copy
# never stops. It wrote ~26 MB past a 76-byte resource and aborted on the first
# unmapped page (Atmosphère report: Data Abort, PC at resource.c:613, both Switch
# configs, first LZSS resource of the run). MSVC and x86/x86-64 GCC already default
# to signed, so the same sources are provably unchanged on the baseline arches: this
# option only brings the console in line with them. Not a resource.c bug to fix
# locally - the codebase writes `(signed char)` explicitly in six places and relies
# on the default elsewhere, so signedness is a property of the build.
add_compile_options(-fsigned-char)
# gnu17, not the compiler default. GCC 14+ defaults to C23, where `()` in a
# declaration means "(void)" - and game_globals_struct.h, which is generated
# and must stay byte-faithful, declares the engine's callbacks as
# `int (__stdcall *g_pGraphicsFunc)();` and calls them with arguments. Under
# C23 that is "too many arguments to function; expected 0, have 3". C17 keeps
# the pre-C23 meaning of `()` as "unspecified parameters", which is what this
# code was written against and what the MSVC and GCC 12 builds still assume.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)
# GCC 14 promoted a pair of C diagnostics from warning to error-by-default:
# -Wincompatible-pointer-types and -Wint-conversion. The decompiled code is full
# of deliberate IDA-typedef puns: audio.c assigns `_DWORD (*)()` (unsigned int)
# function pointers to `int (*)()` globals, text.c round-trips `int (*)(_DWORD)`
# through an `int (*)(int)` local. MSVC accepted all of that silently and GCC 12
# only warned - and these are not bugs to fix, the sources have to stay
# byte-faithful to the original. Demote both back to warnings so they stay
# visible without breaking the build; a no-op on GCC 12 and older. The
# diagnostics that point at a real portability gap (implicit function
# declaration, return mismatch) are deliberately NOT demoted - the missing
# _setjmp prototype on newlib was found exactly this way and fixed properly.
add_compile_options(
  $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>
  $<$<COMPILE_LANGUAGE:C>:-Wno-error=int-conversion>
)
if(NOT PLATFORM_SWITCH)
  # -rdynamic: symbol names in crash backtraces. The Switch links a static PIE
  # through switch.specs, where exporting the dynamic symbol table is neither
  # supported nor useful.
  add_link_options(-rdynamic)
endif()

if(PILOTS_M32)
  # pkg-config must pick the i386 .pc files (multiarch), not the amd64 host ones
  set(ENV{PKG_CONFIG_PATH}
      "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  add_compile_options(-m32)
  add_link_options(-m32)
endif()

if(PILOTS_DEBUG_LOG)
  add_compile_definitions(_DEBUG)
endif()

set(GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SRC)
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Same explicit list as SRC/build_sdl.bat (test_cast.c / test_wave.c stay out of the build)
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
  # switch_libnx.c is #ifdef __SWITCH__ all the way down and compiles to nothing
  # elsewhere; it is listed everywhere for symmetry, as win32_shim.c is on Windows.
  ${GAME_DIR}/switch_libnx.c
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

if(PILOTS_M32)
  message(STATUS "PILOT_RV: 32-bit (-m32), build dir ${CMAKE_BINARY_DIR}")
elseif(PLATFORM_SWITCH)
  message(STATUS "PILOT_RV: Nintendo Switch (aarch64), build dir ${CMAKE_BINARY_DIR}")
else()
  message(STATUS "PILOT_RV: native ${CMAKE_SYSTEM_PROCESSOR}, build dir ${CMAKE_BINARY_DIR}")
endif()
if(PLATFORM_SWITCH)
  message(STATUS "PILOT_RV: debug log ${PILOTS_DEBUG_LOG}, game data goes to sdmc:/switch/pilots")
else()
  message(STATUS "PILOT_RV: debug log ${PILOTS_DEBUG_LOG}, run with CWD = SRC (the game opens .\\PILOTS.EXE)")
endif()

# ---- Nintendo Switch: wrap the ELF into PILOT_RV.nro ----
# nx_create_nro / nx_generate_nacp come from devkitPro's
# cmake/Platform/NintendoSwitch.cmake, which CMake loads as the platform file
# once the toolchain has set CMAKE_SYSTEM_NAME=NintendoSwitch. elf2nro wants an
# ELF, a NACP and an icon; the icon has to be an uncompressed 128x128 8bpp BMP,
# because it is copied into the .nro verbatim and hbmenu draws it as-is.
#
# assets/switch_icon.bmp is that icon, and unlike SRC/pilots.ico it is IN the
# repository: the game's own icon is a PE resource inside PILOTS.EXE - game data,
# extracted at build time on the user's copy and gitignored - whereas whatever is
# baked into a .nro we hand out, so it has to be art this project owns. It is
# generated, not drawn by hand: scripts/make_switch_icon.ps1 derives it from
# arithmetic (see that file for why), so the binary asset has readable source.
#
# Missing asset degrades to devkitPro's default icon rather than failing the
# build, exactly like the Windows icon does.
if(PLATFORM_SWITCH)
  set(SWITCH_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/switch_icon.bmp")
  if(EXISTS "${SWITCH_ICON}")
    set(SWITCH_ICON_ARGS ICON "${SWITCH_ICON}")
  else()
    set(SWITCH_ICON_ARGS "")
    message(WARNING "PILOT_RV: no assets/switch_icon.bmp, using devkitPro's default icon "
                    "(regenerate: pwsh scripts/make_switch_icon.ps1)")
  endif()

  nx_generate_nacp(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/PILOT_RV.nacp"
                   NAME    "Pilots"
                   AUTHOR  "pilots_re reconstruction"
                   VERSION "1.0.0")
  nx_create_nro(PILOT_RV NACP "${CMAKE_CURRENT_BINARY_DIR}/PILOT_RV.nacp" ${SWITCH_ICON_ARGS})
  message(STATUS "PILOT_RV: nro target PILOT_RV_nro -> ${CMAKE_CURRENT_BINARY_DIR}/PILOT_RV.nro")
  message(STATUS "PILOT_RV: nro icon ${SWITCH_ICON_ARGS}")
endif()
