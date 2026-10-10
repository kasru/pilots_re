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
rem ..\build\Debug\x86 for this bat - one dir per bat, so all four builds keep
rem their own objects and never stomp each other. SRC\ keeps only what the
rem game needs at runtime: the exe, SDL2.dll and the linker PDB/MAP.
rem The compiler PDB (/Fd) follows the objs into the same dir.
set OBJDIR=..\build\Debug\x86

echo === DEBUG BUILD (SDL2, VS2022) ===

if not exist "%OBJDIR%" mkdir "%OBJDIR%"
del /Q "%OBJDIR%\*.obj" 2>nul

rem ---- game sources ----
cl.exe /c /TC /W3 /WX- /Od /Zi /D_DEBUG /DWIN32 /MTd ^
  /Fo%OBJDIR%\ ^
  /wd4996 /wd4113 /wd4047 /wd4024 /wd4133 /wd4028 /wd4029 /wd4090 ^
  /wd4244 /wd4267 /wd4554 /wd4838 /wd4309 /wd4005 /wd4701 ^
  /Fd%OBJDIR%\PILOT_RV.pdb ^
  globals.c animation.c render.c gameloop.c ^
  script.c resource.c saveload.c audio.c ^
  video.c text.c tilemap.c blitting.c ^
  graphics.c main.c fileio.c memory.c ^
  input.c native_funcs.c sdl_backend.c win32_shim.c mci_ffmpeg.c
if errorlevel 1 goto :fail

rem ---- libADLMIDI (third_party/libADLMIDI): MIDI music synth, C++ core + Nuked/DosBox OPL emulators ----
set ADLMIDI_OPTS=/DADLMIDI_DISABLE_MIDI_SEQUENCER /DBWMIDI_DISABLE_XMI_SUPPORT /DADLMIDI_DISABLE_OPAL_EMULATOR /DADLMIDI_DISABLE_JAVA_EMULATOR /DADLMIDI_DISABLE_ESFMU_EMULATOR /DADLMIDI_DISABLE_MAME_OPL2_EMULATOR /DADLMIDI_DISABLE_YMFM_EMULATOR
set ADLMIDI_WD=/wd4996 /wd4244 /wd4267 /wd4554 /wd4309 /wd4005 /wd4701 /wd4267

rem OPL emulation is kept optimized even in debug (/O2 on these files only, debug CRT still /MTd)
cl.exe /c /TP /W3 /WX- /O2 /Zi /MTd /D_DEBUG /EHsc %ADLMIDI_OPTS% %ADLMIDI_WD% /Fo%OBJDIR%\ /Fd%OBJDIR%\PILOT_RV.pdb ^
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
cl.exe /c /TC /W3 /WX- /O2 /Zi /MTd /D_DEBUG %ADLMIDI_OPTS% %ADLMIDI_WD% /Fo%OBJDIR%\ /Fd%OBJDIR%\PILOT_RV.pdb ^
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

link.exe /OUT:PILOT_RV.EXE /SUBSYSTEM:WINDOWS /MACHINE:X86 /NOLOGO ^
  /DEBUG /DYNAMICBASE:NO /FIXED ^
  /PDB:PILOT_RV.pdb /MAP:PILOT_RV.map ^
  kernel32.lib user32.lib gdi32.lib winmm.lib vfw32.lib ^
  comdlg32.lib advapi32.lib shell32.lib ole32.lib oldnames.lib ^
  libcmtd.lib libvcruntimed.lib libucrtd.lib libcpmtd.lib SDL2.lib ^
  %OBJDIR%\*.obj
if errorlevel 1 goto :fail

copy /Y "%SDL2%\lib\x86\SDL2.dll" . >nul

echo.
echo SUCCESS: PILOT_RV.EXE (DEBUG, SDL2, VS2022)
goto :end

:fail
echo.
echo BUILD FAILED
popd
exit /b 1

:end
popd
endlocal
