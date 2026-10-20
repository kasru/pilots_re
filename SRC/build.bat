@echo off
rem Сборка Game Maker Player (PILOTS) компилятором MSVC для x86.
rem   build.bat          — выпуск (оптимизация)
rem   build.bat debug    — отладочная сборка
setlocal

set VCTOOLS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717
set WINSDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set PATH=%VCTOOLS%\bin\Hostx86\x86;%WINSDK%\bin\%SDKVER%\x86;%PATH%
set INCLUDE=%VCTOOLS%\include;%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\um;%WINSDK%\Include\%SDKVER%\shared
set LIB=%VCTOOLS%\lib\x86;%WINSDK%\Lib\%SDKVER%\ucrt\x86;%WINSDK%\Lib\%SDKVER%\um\x86

rem SDL2 (порт P1): заголовки/библиотека/DLL. /external:W0 гасит предупреждения
rem в заголовках SDL при /W4 /WX; /external:I добавляет путь поиска как внешний.
set SDL2=%~dp0..\third_party\SDL2

pushd "%~dp0"
if not exist out mkdir out

if /i "%1"=="debug" (
    set CFLAGS=/Od /Zi /MTd /D_DEBUG
    set LFLAGS=/DEBUG
) else (
    set CFLAGS=/O2 /MT /DNDEBUG
    set LFLAGS=
)

cl /nologo /std:c17 /W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS %CFLAGS% /external:W0 /external:I "%SDL2%\include" /Fo"out\\" /Fd"out\\" /c *.c
if errorlevel 1 goto :fail

rc /nologo /fo out\pilots.res pilots.rc
if errorlevel 1 goto :fail

link /nologo /OUT:out\PILOTS_RE.EXE /SUBSYSTEM:WINDOWS /MACHINE:X86 %LFLAGS% /LIBPATH:"%SDL2%\lib\x86" ^
    out\*.obj out\pilots.res kernel32.lib user32.lib gdi32.lib winmm.lib SDL2.lib
if errorlevel 1 goto :fail

copy /y "%SDL2%\lib\x86\SDL2.dll" out\ >nul

echo SUCCESS: out\PILOTS_RE.EXE
popd
exit /b 0

:fail
echo BUILD FAILED
popd
exit /b 1
