# Build for the "Братья Пилоты" reconstruction (port-main line): Linux, Windows
# and the Nintendo Switch, all driven by this one file. SRC/build-vs2022.bat is
# untouched and keeps working - it is the proven Windows baseline, and this
# Windows branch below deliberately mirrors its flags rather than inventing new
# ones (see the WIN32 block).
#
# Flavours:
#
#   cmake -S . -B build32 -DPILOTS_M32=ON    32-bit (-m32) - the proven baseline
#   cmake -S . -B build64                    native (x86_64) - the LP64 layout
#                                             of the generated game-struct headers
#
#   Windows (MSVC), the arch comes from the generator, NOT from a switch here:
#     cmake -S . -B build-win64 -A x64        -> PILOTS_RE_x64.EXE
#     cmake -S . -B build-win32 -A Win32      -> PILOTS_RE.EXE
#     cmake --build build-win64 --config Release
#   or under a vcvars prompt (Ninja picks cl.exe up from the environment):
#     cmake -S . -B build-win64 -G Ninja -DCMAKE_BUILD_TYPE=Release
#   CMAKE_SIZEOF_VOID_P is what selects SDL2's lib/x64 vs lib/x86, so both
#   generators need no separate code path - the target's pointer width is the
#   only thing that matters, and it is what SDL2.lib must match.
#
# The Nintendo Switch .nro step is at the bottom and the console runtime
# lives in switch_libnx.c / __SWITCH__ branches of the port layer.
#
# Keeping the build dirs separate matters: SDL2 and libavcodec must come from
# the matching architecture, so a build dir is tied to one of them for good.
cmake_minimum_required(VERSION 3.16)

# Must be decided before project(): the Switch toolchain file sets
# CMAKE_SYSTEM_NAME=NintendoSwitch, which is what selects Platform/*.cmake and
# brings nx_create_nro() into scope for the .nro step at the bottom.
option(PLATFORM_SWITCH "Cross-compile the Nintendo Switch .nro (devkitA64/libnx)" OFF)

project(PILOTS_RE C CXX)

if(MSVC)
  # ---- MSVC setup, mirroring SRC/build-vs2022.bat ----
  # The .bat stays the proven Windows baseline and keeps working; this block only
  # makes the CMake path produce the same image, so a CMake x64 exe can be handed
  # to the same tests (they look for PILOTS_RE_x64.EXE, hence the OUTPUT_NAME set
  # further down).
  #
  # Static CRT: the .bat links /MT (and /MTd for debug), not CMake's default /MD,
  # so the exe carries no vcruntime/msvcp dependency - a rebuilt copy can be
  # dropped next to the game data anywhere.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # CMake's own MSVC flags carry /W3 (CMAKE_C_FLAGS_INIT); the .bat builds at
  # /W4 /WX and the sources are clean there, so drop the default instead of
  # letting cl.exe warn about being overridden (D9025).
  string(REGEX REPLACE "/W[0-4]" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  # /utf-8: the sources are UTF-8 (verified over all 20 .c files) but the machine
  # code page is not, and every Cyrillic byte lives in a comment or a string -
  # without it cl.exe reads them as CP1251, which is both wrong and, for a byte
  # pair that happens to look like a comment terminator, not merely cosmetic.
  # build-vs2022.bat passes the same flag.
  add_compile_options(/W4 /WX /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Where the game sources and the vendored third_party trees live. Defined before
# the Windows block below needs them (the icon check), and used everywhere else.
set(GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SRC)
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(MSVC)
  # The icon is SRC/pilots.rc -> pilots.ico, and hosting it needs rc.exe from the
  # Windows SDK plus the RC language switched on. The Visual Studio generator
  # does that by itself; Ninja does not, so ask for it here - and only when there
  # is an icon to compile, so a checkout without the game data still configures
  # and builds (host.c then just has no custom exe icon).
  if(EXISTS "${GAME_DIR}/pilots.ico")
    enable_language(RC)
  endif()
endif()

# Debug logging on by default: DBG_* macros are our only diagnostics on Linux
# (writes PILOTS_RE_debug.log next to the exe, backtrace on crash). Off by default
# on the Switch, where the reason is app0:: a shipped NSO has its own filesystem
# mounted read-only, so "next to the exe" cannot be written.
#
# An NRO is the other case, and the one we actually run: libnx chdir's to the
# directory the .nro was loaded from (fs_dev.c __libnx_init_cwd, for anything
# that is not an NSO), and that directory is on sdmc. GetModuleFileNameA has no
# /proc/self/exe on newlib, so it answers the relative "PILOTS_RE.EXE"
# (win32_shim.c), debug_log.h turns that into "PILOTS_RE_debug.log" in the CWD -
# i.e. right next to the .nro and the game data in /switch/pilots. So the log is
# not only possible on the console, it is the only way to see anything there:
# scripts/build_switch_podman.sh debug turns it on.
if(PLATFORM_SWITCH)
  option(PILOTS_DEBUG_LOG "Define _DEBUG (debug log + crash handlers)" OFF)
else()
  option(PILOTS_DEBUG_LOG "Define _DEBUG (debug log + crash handlers)" ON)
endif()
option(PILOTS_M32 "Build 32-bit (-m32) instead of native" OFF)
# Non-Windows only: link SDL2 statically so the exe is one self-contained file
# (macOS i386: without it the image wants @rpath/libSDL2-2.0.0.dylib, which no
# other machine has - the game data sits next to the exe and a dylib would have
# to travel with it). The private deps a static SDL2 drags in (Cocoa & friends,
# iconv) come from `pkg-config --static --libs sdl2`, so an SDL2 built without
# its .a is caught at configure time below, not at link time.
option(PILOTS_STATIC_SDL2 "Link SDL2 statically (self-contained exe, non-Windows)" OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(NOT MSVC)
  # -fno-strict-aliasing: the decompiled code type-puns through IDA macros (bmi/pwh overlays)
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
  # on the default elsewhere, so signedness is a property of the build. MSVC is in
  # the "already signed" half of that sentence (it has no -fsigned-char flag at all,
  # and /J would make it unsigned), hence the guard around both options.
  add_compile_options(-fno-strict-aliasing -fcommon -fsigned-char)
endif()
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
if(NOT MSVC)
  add_compile_options(
    $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>
    $<$<COMPILE_LANGUAGE:C>:-Wno-error=int-conversion>
  )
endif()
if(NOT PLATFORM_SWITCH AND NOT WIN32)
  # -rdynamic: symbol names in crash backtraces. The Switch links a static PIE
  # through switch.specs, where exporting the dynamic symbol table is neither
  # supported nor useful - and MSVC has no such option at all, it has no dynamic
  # symbol table to export in the first place.
  add_link_options(-rdynamic)
endif()

if(PILOTS_M32)
  if(WIN32)
    # -m32 is a GCC driver flag; MSVC picks the arch from the generator instead
    # (-A Win32 vs -A x64, or the vcvars prompt under Ninja). Passing it here
    # would just be passed on to cl.exe as an unknown option.
    message(STATUS "PILOTS_M32 is a GCC/Linux switch - on Windows pick the arch with "
                   "-A Win32 (or -A x64) instead; ignoring it")
  else()
    # pkg-config must pick the i386 .pc files (multiarch), not the amd64 host ones
    set(ENV{PKG_CONFIG_PATH}
        "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    add_compile_options(-m32)
    add_link_options(-m32)
  endif()
endif()

if(PILOTS_DEBUG_LOG)
  if(MSVC)
    # On MSVC _DEBUG has to follow the configuration's CRT, because debug_log.h
    # does not only log: under _WIN32 its DbgLogInit calls
    # _CrtSetReportMode(_CRT_ASSERT, 0) to stop the debug CRT firing assertion
    # popups on invalid handles, and that function exists in the debug CRT only.
    # Defining _DEBUG in a Release build therefore fails at link with
    # LNK2019 _CrtSetReportMode, which is exactly what the first CMake attempt
    # did. build-vs2022.bat never allows the combination either: its debug
    # flavour is /Od /Zi /MTd /D_DEBUG and its release one /O2 /MT /DNDEBUG, so
    # /D_DEBUG is always paired with the /MTd runtime. A generator expression
    # keeps that pairing - the log appears in a Debug build, which is also what
    # tests\windows\x86check.ps1 wants (it runs the debug image and greps
    # <exe>_debug.log).
    add_compile_definitions("$<$<CONFIG:Debug>:_DEBUG>")
  else()
    add_compile_definitions(_DEBUG)
  endif()
endif()

# The port-main file set (the .bat builds compile *.c, CMake lists explicitly).
set(GAME_SOURCES
  ${GAME_DIR}/audio.c
  ${GAME_DIR}/data.c
  ${GAME_DIR}/frame.c
  ${GAME_DIR}/game.c
  ${GAME_DIR}/heap.c
  ${GAME_DIR}/host.c
  ${GAME_DIR}/load.c
  ${GAME_DIR}/movie.c
  ${GAME_DIR}/natives.c
  ${GAME_DIR}/objects.c
  ${GAME_DIR}/path.c
  ${GAME_DIR}/rules.c
  ${GAME_DIR}/save.c
  ${GAME_DIR}/sdl_backend.c
  # switch_libnx.c is #ifdef __SWITCH__ all the way down and compiles to nothing
  # elsewhere; it is listed everywhere for symmetry, as win32_shim.c is.
  ${GAME_DIR}/switch_libnx.c
  ${GAME_DIR}/text.c
  ${GAME_DIR}/vm.c
  ${GAME_DIR}/wave.c
  ${GAME_DIR}/win32_shim.c
  ${GAME_DIR}/mci_ffmpeg.c
)

add_executable(PILOTS_RE ${GAME_SOURCES})
target_include_directories(PILOTS_RE PRIVATE ${GAME_DIR})
if(NOT WIN32)
  # Fake <windows.h>/<mmsystem.h>/<io.h>/<vfw.h>/<intrin.h>/<crtdbg.h> -> win32_shim.h
  # ONLY off Windows: on Linux and the Switch these stand in for the SDK headers
  # the game sources still include verbatim. On Windows the real SDK headers are
  # what the .bat builds have always used, and adding this dir to the path would
  # shadow them - the fake windows.h has no HMIDIOUT, so gm.h's winmm globals
  # (g_hMidiOut, g_hWaveOut, WAVEHDR, ...) stop parsing on the first one.
  target_include_directories(PILOTS_RE PRIVATE ${GAME_DIR}/winshim)
endif()

if(WIN32)
  # ---- SDL2 on Windows: the vendored dev tree, exactly as SRC/build-vs2022.bat ----
  # third_party/SDL2 carries headers plus one import lib per arch
  # (lib/x86/SDL2.lib, lib/x64/SDL2.lib). There is no pkg-config here and ffmpeg
  # is not wanted at all: mci_ffmpeg.c is `#ifndef _WIN32` down to its last line,
  # because video goes through the real MCI on Windows - so libavcodec/libavformat/
  # libavutil/libswscale, which the Linux and Switch builds need for exactly that
  # file, must NOT be linked in or the exe would drag in four libs for a TU that
  # compiles to nothing.
  set(SDL2_ROOT ${THIRD_PARTY}/SDL2)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SDL2_ARCH_DIR x64)
  else()
    set(SDL2_ARCH_DIR x86)
  endif()
  set(SDL2_LIB ${SDL2_ROOT}/lib/${SDL2_ARCH_DIR}/SDL2.lib)
  if(NOT EXISTS "${SDL2_LIB}")
    message(FATAL_ERROR "PILOTS_RE: ${SDL2_LIB} not found - the ${SDL2_ARCH_DIR} SDL2 "
                        "import lib is required (got it from third_party/SDL2)")
  endif()
  # SYSTEM: SDL2's own headers are not our sources, so at /W4 /WX they must not
  # contribute a single warning (build-vs2022.bat passes /external:W0 for the same
  # reason).
  target_include_directories(PILOTS_RE SYSTEM PRIVATE ${SDL2_ROOT}/include)
  # winmm is the midiOut + timeSetEvent half of the audio path (the .bat links it
  # too): on Windows midiOutShortMsg/timeSetEvent are the real winmm calls and
  # SdlMidi_* is compiled out by #ifndef _WIN32, so there is no libADLMIDI below.
  target_link_libraries(PILOTS_RE PRIVATE ${SDL2_LIB} winmm)

  # The Windows exe entry is WinMain (host.c), so /SUBSYSTEM:WINDOWS.
  set_target_properties(PILOTS_RE PROPERTIES WIN32_EXECUTABLE TRUE)
  # Same names as the .bat puts in out\x86 / out\x64, so the tests in tests\
  # (audio.ps1, clickskip.ps1, datapath.ps1, ...) run against a CMake build
  # without knowing where it came from: x86 -> PILOTS_RE.EXE, x64 ->
  # PILOTS_RE_x64.EXE. The build dir already keeps the two apart; the name is
  # what the process filter matches on.
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(PILOTS_RE PROPERTIES OUTPUT_NAME PILOTS_RE_x64)
    # The game stores addresses in 32-bit int and its static buffers must fit a
    # signed int32, so the 64-bit image is pinned below 2 GB with ASLR off -
    # exactly what build-vs2022.bat does for its x64 flavour. x86 needs neither
    # (the loader puts it there anyway), and the .bat passes nothing there either.
    target_link_options(PILOTS_RE PRIVATE /BASE:0x10000000 /DYNAMICBASE:NO)
  else()
    set_target_properties(PILOTS_RE PROPERTIES OUTPUT_NAME PILOTS_RE)
  endif()

  # The icon: a PE resource in the game's own PILOTS.EXE, carved out as
  # SRC/pilots.ico (gitignored like the rest of the game data). pilots.rc is only
  # that one ICON statement. Nothing in the engine loads it - host.c defines
  # HOST_ICON_RESOURCE = 100 to match the .rc and then never calls LoadIcon, and
  # SDL never calls SDL_SetWindowIcon - so it is the shell (Explorer, Alt-Tab,
  # the taskbar) that picks the first icon group out of the image, and without
  # the .ico the exe simply gets no icon of its own. That is why a missing file
  # is a message and not an error.
  if(EXISTS "${GAME_DIR}/pilots.ico")
    # rc.exe resolves the .ico relative to its own CWD, not to the .rc, so hand
    # it SRC explicitly - under Ninja the CWD is the build dir.
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${GAME_DIR}\"")
    target_sources(PILOTS_RE PRIVATE ${GAME_DIR}/pilots.rc)
  else()
    message(STATUS "PILOTS_RE: no SRC/pilots.ico, building without an exe icon "
                   "(only Explorer/Alt-Tab will notice, the game itself does not load it)")
  endif()

  # SDL2.dll next to the exe: both arches ship the same DLL name, and each build
  # dir gets its own copy so the x86 and x64 outputs stay independently runnable.
  add_custom_command(POST_BUILD TARGET PILOTS_RE COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     "${SDL2_ROOT}/lib/${SDL2_ARCH_DIR}/SDL2.dll" "$<TARGET_FILE_DIR:PILOTS_RE>")
  message(STATUS "PILOTS_RE: SDL2 from ${SDL2_ROOT}/lib/${SDL2_ARCH_DIR} "
                 "(${CMAKE_SIZEOF_VOID_P}-byte pointers)")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)

  if(PILOTS_STATIC_SDL2)
    # A framework reference from pkg-config arrives as the two tokens
    # "-framework" "X". Kept as-is, those confuse CMake's link-flag handling:
    # it de-duplicates the repeated "-framework" and orphans the names
    # ("clang: error: no such file or directory: 'CoreFoundation'"). Collapsing
    # each pair into one -Wl,-framework,X token (the form SDL2's own
    # Libs.private already uses) makes every framework reference self-contained
    # and dedup-safe - AV and SDL2 both name CoreVideo, which then simply
    # collapses into one copy.
    function(pilots_merge_frameworks OUT)
      set(result "")
      set(pending "")
      foreach(flag IN LISTS ARGN)
        if(pending)
          list(APPEND result "-Wl,-framework,${flag}")
          set(pending "")
        elseif(flag STREQUAL "-framework")
          set(pending 1)
        else()
          list(APPEND result "${flag}")
        endif()
      endforeach()
      set(${OUT} ${result} PARENT_SCOPE)
    endfunction()

    # Explicit archive, not -lSDL2: the linker prefers the dylib sitting next to
    # libSDL2.a, which is exactly what this option exists to avoid. The .a is
    # also how the arch is pinned (a fat SDL2 would silently pick its slice).
    set(SDL2_STATIC_LIB ${SDL2_LIBDIR}/libSDL2.a)
    if(NOT EXISTS "${SDL2_STATIC_LIB}")
      message(FATAL_ERROR "PILOTS_RE: ${SDL2_STATIC_LIB} not found - "
                          "PILOTS_STATIC_SDL2 needs an SDL2 built with static libs")
    endif()
    # Everything a static SDL2 references but that -lSDL2 would have carried
    # via the dylib's own install name (Cocoa, CoreAudio, iconv, ...). This is
    # `Libs.private` from sdl2.pc - the list SDL2 itself declares.
    execute_process(COMMAND pkg-config --static --libs sdl2
                    OUTPUT_VARIABLE SDL2_PRIVATE_LIBS
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(SDL2_PRIVATE_LIBS_LIST UNIX_COMMAND "${SDL2_PRIVATE_LIBS}")
    list(REMOVE_ITEM SDL2_PRIVATE_LIBS_LIST -lSDL2)  # that is what we just pinned
    # Same for AV, but linked by hand: PkgConfig::AV stays the source of the
    # headers, and its INTERFACE (imported targets put raw flags there) is what
    # the framework-merging above has to dodge.
    execute_process(COMMAND pkg-config --libs libavcodec libavformat libavutil libswscale
                    OUTPUT_VARIABLE AV_LIBS
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(AV_LIBS_LIST UNIX_COMMAND "${AV_LIBS}")
    pilots_merge_frameworks(SDL2_PRIVATE_LIBS_LIST ${SDL2_PRIVATE_LIBS_LIST})
    pilots_merge_frameworks(AV_LIBS_LIST ${AV_LIBS_LIST})
    target_compile_options(PILOTS_RE PRIVATE ${SDL2_CFLAGS_OTHER} ${AV_CFLAGS_OTHER})
    target_include_directories(PILOTS_RE SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS} ${AV_INCLUDE_DIRS})
    target_link_libraries(PILOTS_RE PRIVATE ${SDL2_STATIC_LIB} ${SDL2_PRIVATE_LIBS_LIST} ${AV_LIBS_LIST} m)
    message(STATUS "PILOTS_RE: static SDL2 ${SDL2_STATIC_LIB}")
  else()
    target_link_libraries(PILOTS_RE PRIVATE PkgConfig::SDL2 PkgConfig::AV m)
  endif()
endif()

if(NOT WIN32 AND NOT PLATFORM_SWITCH AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  # Linux x86_64 -> PILOTS_RE_x64 (build32's 32-bit baseline keeps plain
  # PILOTS_RE, same rule as on Windows). The Switch is also 8-byte pointers
  # but stays PILOTS_RE.nro - its name has no arch postfix by design.
  set_target_properties(PILOTS_RE PROPERTIES OUTPUT_NAME PILOTS_RE_x64)
endif()

# ---- libADLMIDI: the OPL3 synth under midiOut (the music) ----
# NOT built on Windows: there midiOutShortMsg/timeSetEvent are real winmm calls
# (build-vs2022.bat links winmm), SdlMidi_* is #ifndef _WIN32, and sdl_backend.c
# does not even include adlmidi.h - so the whole library, and the GCC-only -O2 it
# is compiled with below, is Linux and Switch only.
if(NOT WIN32)
# Source list and defines as the sdl2 branch's .bat/CMake builds (tag v0.1): the
# sequencer and every emulator except Nuked/DOSBox are compiled out - the game
# feeds realtime events through midiOutShortMsg and renders with adl_generate,
# so no sequencer path ever runs.
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
# The lib stays optimized in a Debug game build too (the .bat builds gave it
# /O2 against a /Od game): Nuked OPL3 is cycle-accurate and this is audio
# rendered on a 10 ms timer - an unoptimized debug build is where music would
# start underrunning while everything else looks fine.
target_compile_options(adlmidi PRIVATE -O2)

target_link_libraries(PILOTS_RE PRIVATE adlmidi)
endif() # NOT WIN32

if(PILOTS_M32)
  message(STATUS "PILOTS_RE: 32-bit (-m32), build dir ${CMAKE_BINARY_DIR}")
elseif(PLATFORM_SWITCH)
  message(STATUS "PILOTS_RE: Nintendo Switch (aarch64), build dir ${CMAKE_BINARY_DIR}")
elseif(WIN32)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "PILOTS_RE: Windows x64, build dir ${CMAKE_BINARY_DIR}")
  else()
    message(STATUS "PILOTS_RE: Windows x86, build dir ${CMAKE_BINARY_DIR}")
  endif()
else()
  message(STATUS "PILOTS_RE: native ${CMAKE_SYSTEM_PROCESSOR}, build dir ${CMAKE_BINARY_DIR}")
endif()
if(PLATFORM_SWITCH)
  message(STATUS "PILOTS_RE: debug log ${PILOTS_DEBUG_LOG}, game data goes to sdmc:/switch/pilots")
elseif(WIN32)
  message(STATUS "PILOTS_RE: debug log ${PILOTS_DEBUG_LOG}, run the exe from a dir "
                 "holding the original PILOTS.EXE (the engine opens .\\PILOTS.EXE)")
else()
  message(STATUS "PILOTS_RE: debug log ${PILOTS_DEBUG_LOG}, run with CWD = SRC (the game opens .\\PILOTS.EXE)")
endif()

# ---- Nintendo Switch: wrap the ELF into PILOTS_RE.nro ----
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
    message(WARNING "PILOTS_RE: no assets/switch_icon.bmp, using devkitPro's default icon "
                    "(regenerate: pwsh scripts/make_switch_icon.ps1)")
  endif()

  nx_generate_nacp(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/PILOTS_RE.nacp"
                   NAME    "Pilots"
                   AUTHOR  "pilots_re reconstruction"
                   VERSION "1.0.0")
  nx_create_nro(PILOTS_RE NACP "${CMAKE_CURRENT_BINARY_DIR}/PILOTS_RE.nacp" ${SWITCH_ICON_ARGS})
  message(STATUS "PILOTS_RE: nro target PILOTS_RE_nro -> ${CMAKE_CURRENT_BINARY_DIR}/PILOTS_RE.nro")
  message(STATUS "PILOTS_RE: nro icon ${SWITCH_ICON_ARGS}")
endif()
