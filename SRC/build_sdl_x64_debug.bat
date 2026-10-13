@echo off
setlocal

rem x64 DEBUG build: same as build_sdl_x64.bat but with /D_DEBUG /MTd /Zi so
rem the runtime writes PILOT_RV_debug.log and a register/stack dump on crash.
rem Kept separate so the proven x86 bats stay byte-identical. No /FIXED (x64
rem image keeps base relocs); /DYNAMICBASE:NO still pins the base.
rem
rem The binary is named PILOT_RV_x64.EXE so it can sit next to the x86 build
rem without overwriting it.

set VCTOOLS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set WINSDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set PATH=%VCTOOLS%\bin\Hostx64\x64;%WINSDK%\bin\%SDKVER%\x64;%PATH%
set INCLUDE=%VCTOOLS%\include;%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\um;%WINSDK%\Include\%SDKVER%\shared
set LIB=%VCTOOLS%\lib\x64;%WINSDK%\Lib\%SDKVER%\ucrt\x64;%WINSDK%\Lib\%SDKVER%\um\x64

pushd "%~dp0"

set SDL2=..\third_party\SDL2
set ADLMIDI=..\third_party\libADLMIDI
set INCLUDE=%INCLUDE%;%SDL2%\include;%ADLMIDI%\include
set LIB=%LIB%;%SDL2%\lib\x64

rem Intermediate .obj files go to ..\build\<Configuration>\<Platform> - e.g.
rem ..\build\Debug\x64 for this bat - one dir per bat, so all four builds keep
rem their own objects and never stomp each other. SRC\ keeps only what the
rem game needs at runtime: the exe, SDL2.dll and the linker PDB/MAP.
rem The compiler PDB (/Fd) follows the objs into the same dir.
set OBJDIR=..\build\Debug\x64

echo === X64 DEBUG BUILD (SDL2, VS2022, Hostx64) ===

if not exist "%OBJDIR%" mkdir "%OBJDIR%"
del /Q "%OBJDIR%\*.obj" 2>nul

rem ---- game sources ----
cl.exe /c /TC /W3 /WX- /Od /Zi /D_DEBUG /DWIN32 /MTd ^
  /Fo%OBJDIR%\ ^
  /wd4996 /wd4113 /wd4047 /wd4024 /wd4133 /wd4028 /wd4029 /wd4090 ^
  /wd4244 /wd4267 /wd4554 /wd4838 /wd4309 /wd4005 /wd4701 ^
  /Fd%OBJDIR%\PILOT_RV_x64.pdb ^
  globals.c animation.c render.c gameloop.c ^
  script.c resource.c saveload.c audio.c ^
  video.c text.c tilemap.c blitting.c ^
  graphics.c main.c fileio.c memory.c ^
  input.c native_funcs.c sdl_backend.c win32_shim.c mci_ffmpeg.c
if errorlevel 1 goto :fail

rem ---- libADLMIDI ----
set ADLMIDI_OPTS=/DADLMIDI_DISABLE_MIDI_SEQUENCER /DBWMIDI_DISABLE_XMI_SUPPORT /DADLMIDI_DISABLE_OPAL_EMULATOR /DADLMIDI_DISABLE_JAVA_EMULATOR /DADLMIDI_DISABLE_ESFMU_EMULATOR /DADLMIDI_DISABLE_MAME_OPL2_EMULATOR /DADLMIDI_DISABLE_YMFM_EMULATOR
set ADLMIDI_WD=/wd4996 /wd4244 /wd4267 /wd4554 /wd4309 /wd4005 /wd4701 /wd4267

cl.exe /c /TP /W3 /WX- /O2 /Zi /MTd /D_DEBUG /EHsc %ADLMIDI_OPTS% %ADLMIDI_WD% /Fo%OBJDIR%\ /Fd%OBJDIR%\PILOT_RV_x64.pdb ^
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

cl.exe /c /TC /W3 /WX- /O2 /Zi /MTd /D_DEBUG %ADLMIDI_OPTS% %ADLMIDI_WD% /Fo%OBJDIR%\ /Fd%OBJDIR%\PILOT_RV_x64.pdb ^
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
  rc.exe /nologo /fo "%OBJDIR%\pilots.res" pilots.rc
  if errorlevel 1 goto :fail
  set ICONRES=%OBJDIR%\pilots.res
) else (
  echo WARNING: no icon - scripts\extract_icon.ps1 could not carve it out of PILOTS.EXE
  set ICONRES=
)

link.exe /OUT:PILOT_RV_x64.EXE /SUBSYSTEM:WINDOWS /MACHINE:X64 /NOLOGO ^
  /DEBUG /DYNAMICBASE:NO ^
  /PDB:PILOT_RV_x64.pdb /MAP:PILOT_RV_x64.map ^
  kernel32.lib user32.lib gdi32.lib winmm.lib vfw32.lib ^
  comdlg32.lib advapi32.lib shell32.lib ole32.lib oldnames.lib ^
  libcmtd.lib libvcruntimed.lib libucrtd.lib libcpmtd.lib SDL2.lib ^
  %ICONRES% %OBJDIR%\*.obj
if errorlevel 1 goto :fail

copy /Y "%SDL2%\lib\x64\SDL2.dll" . >nul

echo.
echo SUCCESS: PILOT_RV_x64.EXE (X64 DEBUG, SDL2, VS2022)
goto :end

:fail
echo.
echo BUILD FAILED
popd
exit /b 1

:end
popd
endlocal
