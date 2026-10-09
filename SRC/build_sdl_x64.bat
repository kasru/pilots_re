@echo off
setlocal

rem x64 build of the same sources as build_sdl_release.bat (Hostx64 toolset).
rem Kept separate so the proven x86 bats stay byte-identical. This is the
rem 64-bit milestone on the way to a Nintendo Switch (aarch64) build: it
rem surfaces every "pointer lives in int" site of the decompiled code under
rem MSVC x64 (C4311/C4312) and proves the fixes run natively.
rem
rem Unlike x86, no /FIXED here: the x64 image must keep its base relocs
rem (the loader/backups depend on them); /DYNAMICBASE:NO still pins the base.

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

echo === X64 BUILD (SDL2, VS2022, Hostx64) ===

del /Q *.obj 2>nul

rem ---- game sources ----
cl.exe /c /TC /W3 /WX- /Od /DNDEBUG /DWIN32 /MT ^
  /wd4996 /wd4113 /wd4047 /wd4024 /wd4133 /wd4028 /wd4029 /wd4090 ^
  /wd4244 /wd4267 /wd4554 /wd4838 /wd4309 /wd4005 /wd4701 ^
  globals.c animation.c render.c gameloop.c ^
  script.c resource.c saveload.c audio.c ^
  video.c text.c tilemap.c blitting.c ^
  graphics.c main.c fileio.c memory.c ^
  input.c native_funcs.c sdl_backend.c win32_shim.c mci_ffmpeg.c
if errorlevel 1 goto :fail

rem ---- libADLMIDI (third_party/libADLMIDI): MIDI music synth, C++ core + Nuked/DosBox OPL emulators ----
set ADLMIDI_OPTS=/DADLMIDI_DISABLE_MIDI_SEQUENCER /DBWMIDI_DISABLE_XMI_SUPPORT /DADLMIDI_DISABLE_OPAL_EMULATOR /DADLMIDI_DISABLE_JAVA_EMULATOR /DADLMIDI_DISABLE_ESFMU_EMULATOR /DADLMIDI_DISABLE_MAME_OPL2_EMULATOR /DADLMIDI_DISABLE_YMFM_EMULATOR
set ADLMIDI_WD=/wd4996 /wd4244 /wd4267 /wd4554 /wd4309 /wd4005 /wd4701 /wd4267

cl.exe /c /TP /W3 /WX- /O2 /MT /EHsc %ADLMIDI_OPTS% %ADLMIDI_WD% ^
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
cl.exe /c /TC /W3 /WX- /O2 /MT %ADLMIDI_OPTS% %ADLMIDI_WD% ^
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

link.exe /OUT:PILOT_RV.EXE /SUBSYSTEM:WINDOWS /MACHINE:X64 /NOLOGO ^
  /DEBUG /DYNAMICBASE:NO ^
  /PDB:PILOT_RV.pdb /MAP:PILOT_RV.map ^
  kernel32.lib user32.lib gdi32.lib winmm.lib vfw32.lib ^
  comdlg32.lib advapi32.lib shell32.lib ole32.lib oldnames.lib ^
  libcmt.lib libvcruntime.lib libucrt.lib libcpmt.lib SDL2.lib ^
  *.obj
if errorlevel 1 goto :fail

copy /Y "%SDL2%\lib\x64\SDL2.dll" . >nul

echo.
echo SUCCESS: PILOT_RV.EXE (X64, SDL2, VS2022)
goto :end

:fail
echo.
echo BUILD FAILED
popd
exit /b 1

:end
popd
endlocal
