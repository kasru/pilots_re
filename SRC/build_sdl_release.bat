@echo off
setlocal

set VCTOOLS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set WINSDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set PATH=%VCTOOLS%\bin\Hostx86\x86;%WINSDK%\bin\%SDKVER%\x86;%PATH%
set INCLUDE=%VCTOOLS%\include;%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\um;%WINSDK%\Include\%SDKVER%\shared
set LIB=%VCTOOLS%\lib\x86;%WINSDK%\Lib\%SDKVER%\ucrt\x86;%WINSDK%\Lib\%SDKVER%\um\x86

pushd "%~dp0"

set SDL2=..\third_party\SDL2
set ADLMIDI=..\third_party\libADLMIDI
set INCLUDE=%INCLUDE%;%SDL2%\include;%ADLMIDI%\include
set LIB=%LIB%;%SDL2%\lib\x86

rem Intermediate .obj files go to ..\build\<Configuration>\<Platform> - e.g.
rem ..\build\Release\x86 for this bat - one dir per bat, so all four builds keep
rem their own objects and never stomp each other. SRC\ keeps only what the
rem game needs at runtime: the exe, SDL2.dll and the linker PDB/MAP.
set OBJDIR=..\build\Release\x86

echo === RELEASE BUILD (SDL2, VS2022) ===

if not exist "%OBJDIR%" mkdir "%OBJDIR%"
del /Q "%OBJDIR%\*.obj" 2>nul

rem ---- game sources ----
cl.exe /c /TC /W3 /WX- /Od /DNDEBUG /DWIN32 /MT ^
  /Fo%OBJDIR%\ ^
  /wd4996 /wd4113 /wd4047 /wd4024 /wd4133 /wd4028 /wd4029 /wd4090 ^
  /wd4244 /wd4267 /wd4554 /wd4838 /wd4309 /wd4005 /wd4701 ^
  globals.c animation.c render.c gameloop.c ^
  script.c resource.c saveload.c audio.c ^
  video.c text.c tilemap.c blitting.c ^
  graphics.c main.c fileio.c memory.c ^
  input.c native_funcs.c sdl_backend.c switch_libnx.c win32_shim.c mci_ffmpeg.c
if errorlevel 1 goto :fail

rem ---- libADLMIDI (third_party/libADLMIDI): MIDI music synth, C++ core + Nuked/DosBox OPL emulators ----
set ADLMIDI_OPTS=/DADLMIDI_DISABLE_MIDI_SEQUENCER /DBWMIDI_DISABLE_XMI_SUPPORT /DADLMIDI_DISABLE_OPAL_EMULATOR /DADLMIDI_DISABLE_JAVA_EMULATOR /DADLMIDI_DISABLE_ESFMU_EMULATOR /DADLMIDI_DISABLE_MAME_OPL2_EMULATOR /DADLMIDI_DISABLE_YMFM_EMULATOR
set ADLMIDI_WD=/wd4996 /wd4244 /wd4267 /wd4554 /wd4309 /wd4005 /wd4701 /wd4267

cl.exe /c /TP /W3 /WX- /O2 /MT /EHsc %ADLMIDI_OPTS% %ADLMIDI_WD% /Fo%OBJDIR%\ ^
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
cl.exe /c /TC /W3 /WX- /O2 /MT %ADLMIDI_OPTS% %ADLMIDI_WD% /Fo%OBJDIR%\ ^
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

link.exe /OUT:PILOT_RV.EXE /SUBSYSTEM:WINDOWS /MACHINE:X86 /NOLOGO ^
  /DEBUG /DYNAMICBASE:NO /FIXED ^
  /PDB:PILOT_RV.pdb /MAP:PILOT_RV.map ^
  kernel32.lib user32.lib gdi32.lib winmm.lib vfw32.lib ^
  comdlg32.lib advapi32.lib shell32.lib ole32.lib oldnames.lib ^
  libcmt.lib libvcruntime.lib libucrt.lib libcpmt.lib SDL2.lib ^
  %ICONRES% %OBJDIR%\*.obj
if errorlevel 1 goto :fail

copy /Y "%SDL2%\lib\x86\SDL2.dll" . >nul

echo.
echo SUCCESS: PILOT_RV.EXE (RELEASE, SDL2, VS2022)
goto :end

:fail
echo.
echo BUILD FAILED
popd
exit /b 1

:end
popd
endlocal
