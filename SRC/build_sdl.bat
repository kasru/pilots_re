@echo off
setlocal enableextensions

rem build_sdl.bat - the one Windows build of the SDL2 port, replacing the four
rem per-combination bats (build_sdl_debug / build_sdl_release / build_sdl_x64 /
rem build_sdl_x64_debug). Usage:
rem
rem     build_sdl.bat [debug^|release] [x86^|x64]        default: release x64
rem
rem debug   = /D_DEBUG /MTd /Zi: writes PILOT_RV[_x64]_debug.log and a
rem           register/stack dump on crash; in release DBG_* are no-ops.
rem x86     = the proven baseline -> PILOT_RV.EXE (+ /FIXED).
rem x64     = the LP64 milestone toward the Switch/aarch64 build ->
rem           PILOT_RV_x64.EXE, a separate name so it can sit next to the x86
rem           build; no /FIXED because the x64 image keeps its base relocs.
rem           Both arches pin the base with /DYNAMICBASE:NO.
rem
rem Intermediate .obj files go to ..\build\<Configuration>\<Platform> - one dir
rem per combination, so all four builds coexist and never stomp each other.
rem SRC\ keeps only runtime + linker outputs: the exe, SDL2.dll and the linker
rem PDB/MAP. Toolchain paths are hardcoded below - edit VCTOOLS/WINSDK/SDKVER
rem there if your install differs.

set VCTOOLS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set WINSDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

rem ---- arg 1: debug | release (default release) ----
set CFG=%~1
if not "%CFG%"=="" goto cfg_given
set CFG=release
:cfg_given
if /I "%CFG%"=="debug" goto cfg_debug
if /I "%CFG%"=="release" goto cfg_release
echo BUILD FAILED: unknown configuration "%CFG%" - use: build_sdl.bat [debug^|release] [x86^|x64]
exit /b 2

:cfg_debug
set CFGDIR=Debug
set CFGNAME=DEBUG
set CFGCL=/D_DEBUG /MTd /Zi
set CFGADL=/MTd /D_DEBUG /Zi
set CFGLIB=libcmtd.lib libvcruntimed.lib libucrtd.lib libcpmtd.lib
set CFGDEBUG=1
goto cfg_done

:cfg_release
set CFGDIR=Release
set CFGNAME=RELEASE
set CFGCL=/DNDEBUG /MT
set CFGADL=/MT
set CFGLIB=libcmt.lib libvcruntime.lib libucrt.lib libcpmt.lib
set CFGDEBUG=
goto cfg_done

:cfg_done

rem ---- arg 2: x86 | x64 (default x64) ----
set PLAT=%~2
if not "%PLAT%"=="" goto plat_given
set PLAT=x64
:plat_given
if /I "%PLAT%"=="x86" goto plat_x86
if /I "%PLAT%"=="x64" goto plat_x64
echo BUILD FAILED: unknown platform "%PLAT%" - use: build_sdl.bat [debug^|release] [x86^|x64]
exit /b 2

:plat_x86
set HOST=Hostx86\x86
set ARCH=x86
set EXE=PILOT_RV.EXE
set PDBBASE=PILOT_RV
set MACH=X86
set RCDEFS=
set LNKFIX=/FIXED
goto plat_done

:plat_x64
set HOST=Hostx64\x64
set ARCH=x64
set EXE=PILOT_RV_x64.EXE
set PDBBASE=PILOT_RV_x64
set MACH=X64
set RCDEFS=/d PILOTS_X64
set LNKFIX=
goto plat_done

:plat_done

set PATH=%VCTOOLS%\bin\%HOST%;%WINSDK%\bin\%SDKVER%\%ARCH%;%PATH%
set INCLUDE=%VCTOOLS%\include;%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\um;%WINSDK%\Include\%SDKVER%\shared
set LIB=%VCTOOLS%\lib\%ARCH%;%WINSDK%\Lib\%SDKVER%\ucrt\%ARCH%;%WINSDK%\Lib\%SDKVER%\um\%ARCH%

pushd "%~dp0"

set SDL2=..\third_party\SDL2
set ADLMIDI=..\third_party\libADLMIDI
set INCLUDE=%INCLUDE%;%SDL2%\include;%ADLMIDI%\include
set LIB=%LIB%;%SDL2%\lib\%ARCH%

rem ..\build\<Configuration>\<Platform> again - the one dir per combination
rem that keeps the four builds from stomping each other. The compiler PDB
rem (/Fd) follows the objs there in debug only; release passes no /Zi.
set OBJDIR=..\build\%CFGDIR%\%ARCH%
set FDPDB=
if not "%CFGDEBUG%"=="" set FDPDB=/Fd%OBJDIR%\%PDBBASE%.pdb

echo === %CFGNAME% BUILD %ARCH% (SDL2, VS2022, %HOST%) ===

if not exist "%OBJDIR%" mkdir "%OBJDIR%"
del /Q "%OBJDIR%\*.obj" 2>nul

rem ---- game sources ----
cl.exe /c /TC /W3 /WX- /Od %CFGCL% /DWIN32 ^
  /Fo%OBJDIR%\ ^
  /wd4996 /wd4113 /wd4047 /wd4024 /wd4133 /wd4028 /wd4029 /wd4090 ^
  /wd4244 /wd4267 /wd4554 /wd4838 /wd4309 /wd4005 /wd4701 ^
  %FDPDB% ^
  globals.c animation.c render.c gameloop.c ^
  script.c resource.c saveload.c audio.c ^
  video.c text.c tilemap.c blitting.c ^
  graphics.c main.c fileio.c memory.c ^
  input.c native_funcs.c sdl_backend.c switch_libnx.c win32_shim.c mci_ffmpeg.c
if errorlevel 1 goto :fail

rem ---- libADLMIDI (third_party/libADLMIDI): MIDI music synth, C++ core + Nuked/DosBox OPL emulators ----
set ADLMIDI_OPTS=/DADLMIDI_DISABLE_MIDI_SEQUENCER /DBWMIDI_DISABLE_XMI_SUPPORT /DADLMIDI_DISABLE_OPAL_EMULATOR /DADLMIDI_DISABLE_JAVA_EMULATOR /DADLMIDI_DISABLE_ESFMU_EMULATOR /DADLMIDI_DISABLE_MAME_OPL2_EMULATOR /DADLMIDI_DISABLE_YMFM_EMULATOR
set ADLMIDI_WD=/wd4996 /wd4244 /wd4267 /wd4554 /wd4309 /wd4005 /wd4701 /wd4267

rem OPL emulation is kept optimized even in debug (/O2 on these files only)
cl.exe /c /TP /W3 /WX- /O2 %CFGADL% /EHsc %ADLMIDI_OPTS% %ADLMIDI_WD% /Fo%OBJDIR%\ %FDPDB% ^
  %ADLMIDI%\src\adlmidi.cpp ^
  %ADLMIDI%\src\adlmidi_load.cpp ^
  %ADLMIDI%\src\adlmidi_midiplay.cpp ^
  %ADLMIDI%\src\adlmidi_opl3.cpp ^
  %ADLMIDI%\src\adlmidi_private.cpp ^
  %ADLMIDI%\src\inst_db.cpp ^
  %ADLMIDI%\src\chips\dosbox_opl2.cpp ^
  %ADLMIDI%\src\chips\dosbox_opl3.cpp ^
  %ADLMIDI%\src\chips\dosbox\dbopl.cpp ^
  %ADLMIDI%\src\chips\nuked_opl3.cpp ^
  %ADLMIDI%\src\chips\nuked_opl3_fast.cpp ^
  %ADLMIDI%\src\chips\nuked_opl2.cpp ^
  %ADLMIDI%\src\chips\nuked_cqm.cpp
if errorlevel 1 goto :fail

rem libADLMIDI C parts: instrument database, volume models, WOPL bank loader, chip cores
cl.exe /c /TC /W3 /WX- /O2 %CFGADL% %ADLMIDI_OPTS% %ADLMIDI_WD% /Fo%OBJDIR%\ %FDPDB% ^
  %ADLMIDI%\src\wopl\wopl_file.c ^
  %ADLMIDI%\src\models\model_ail.c ^
  %ADLMIDI%\src\models\model_apogee.c ^
  %ADLMIDI%\src\models\model_dmx.c ^
  %ADLMIDI%\src\models\model_generic.c ^
  %ADLMIDI%\src\models\model_hmi_sos.c ^
  %ADLMIDI%\src\models\model_msadlib.c ^
  %ADLMIDI%\src\models\model_oconnell.c ^
  %ADLMIDI%\src\models\model_win9x.c ^
  %ADLMIDI%\src\chips\nuked\nukedopl3.c ^
  %ADLMIDI%\src\chips\nuked_fast\nukedopl3_fast.c ^
  %ADLMIDI%\src\chips\nuked\nukedopl2.c ^
  %ADLMIDI%\src\chips\nuked_cqm\cqm.c
if errorlevel 1 goto :fail

rem ---- application icon, carved out of the original PILOTS.EXE ----
rem pilots.ico is deliberately NOT in the repo: it is original game data, like
rem PILOTS.EXE itself, so it is regenerated on demand by ..\scripts\
rem extract_icon.ps1. Without it the exe still links - it just gets the
rem linker's default icon - so a checkout without the game data still builds.
if not exist pilots.ico powershell.exe -NoProfile -ExecutionPolicy Bypass -File ..\scripts\extract_icon.ps1
if exist pilots.ico (
  rc.exe /nologo %RCDEFS% /fo "%OBJDIR%\pilots.res" pilots.rc
  if errorlevel 1 goto :fail
  set ICONRES=%OBJDIR%\pilots.res
) else (
  echo WARNING: no icon - scripts\extract_icon.ps1 could not carve it out of PILOTS.EXE
  set ICONRES=
)

link.exe /OUT:%EXE% /SUBSYSTEM:WINDOWS /MACHINE:%MACH% /NOLOGO ^
  /DEBUG /DYNAMICBASE:NO %LNKFIX% ^
  /PDB:%PDBBASE%.pdb /MAP:%PDBBASE%.map ^
  kernel32.lib user32.lib gdi32.lib winmm.lib vfw32.lib ^
  comdlg32.lib advapi32.lib shell32.lib ole32.lib oldnames.lib ^
  %CFGLIB% SDL2.lib ^
  %ICONRES% %OBJDIR%\*.obj
if errorlevel 1 goto :fail

copy /Y "%SDL2%\lib\%ARCH%\SDL2.dll" . >nul

echo.
echo SUCCESS: %EXE% (%CFGNAME% %ARCH%, SDL2, VS2022)
goto :end

:fail
echo.
echo BUILD FAILED
popd
exit /b 1

:end
popd
endlocal
exit /b 0
