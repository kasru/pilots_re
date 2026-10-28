@echo off
rem Сборка Game Maker Player (PILOTS) компилятором MSVC под VS2022.
rem   build-vs2022.bat           — x86, выпуск (оптимизация)
rem   build-vs2022.bat debug     — x86, отладочная сборка
rem   build-vs2022.bat [debug] x64 — x64-сборка (out\x64\PILOT_RE_x64.EXE);
rem        образ пинится ниже 2 ГБ (/BASE + /DYNAMICBASE:NO): игра хранит
rem        адреса в 32-разрядных int, и статическим буферам (.bss) тоже
rem        нужно помещаться в знаковый int32.
rem Каждая архитектура собирается в свой каталог: x86 -> out\x86, x64 -> out\x64.
rem Раньше всё падало плоско в out\, и x86/x64 перетирали друг друга (общая
rem SDL2.dll), так что запустить можно было только последнюю собранную.
rem То же, что build.bat, но toolset VS2022 (14.44) вместо VS18 (14.50):
rem на этой машине установлен только VS2022 Community.
setlocal

set VCTOOLS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set WINSDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set ARCH=x86
set HOSTDIR=Hostx86\x86
set MACH=X86
set EXENAME=PILOT_RE.EXE
set OUTDIR=out\x86
set LNKPLAT=
set PLATARG=%~2
if /i "%~1"=="x64" set PLATARG=%~1
if /i "%PLATARG%"=="x64" (
    set ARCH=x64
    set HOSTDIR=Hostx64\x64
    set MACH=X64
    set EXENAME=PILOT_RE_x64.EXE
    set OUTDIR=out\x64
    set LNKPLAT=/BASE:0x10000000 /DYNAMICBASE:NO
)

set PATH=%VCTOOLS%\bin\%HOSTDIR%;%WINSDK%\bin\%SDKVER%\%ARCH%;%PATH%
set INCLUDE=%VCTOOLS%\include;%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\um;%WINSDK%\Include\%SDKVER%\shared
set LIB=%VCTOOLS%\lib\%ARCH%;%WINSDK%\Lib\%SDKVER%\ucrt\%ARCH%;%WINSDK%\Lib\%SDKVER%\um\%ARCH%

rem SDL2: заголовки/библиотека/ DLL. /external:W0 гасит предупреждения
rem в заголовках SDL при /W4 /WX; /external:I добавляет путь поиска как внешний.
set SDL2=%~dp0..\third_party\SDL2

pushd "%~dp0"
rem Один каталог вывода на архитектуру: x86 и x64 иначе перетирают друг друга
rem (общая SDL2.dll лежит в обоих), и запустить можно только последнюю сборку.
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

if /i "%1"=="debug" (
    set CFLAGS=/Od /Zi /MTd /D_DEBUG
    set LFLAGS=/DEBUG
) else (
    set CFLAGS=/O2 /MT /DNDEBUG
    set LFLAGS=
)

cl /nologo /std:c17 /W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS %CFLAGS% /external:W0 /external:I "%SDL2%\include" /Fo"%OUTDIR%\\" /Fd"%OUTDIR%\\" /c *.c
if errorlevel 1 goto :fail

rc /nologo /fo "%OUTDIR%\pilots.res" pilots.rc
if errorlevel 1 goto :fail

link /nologo /OUT:"%OUTDIR%\%EXENAME%" /SUBSYSTEM:WINDOWS /MACHINE:%MACH% %LNKPLAT% %LFLAGS% /LIBPATH:"%SDL2%\lib\%ARCH%" ^
    "%OUTDIR%\*.obj" "%OUTDIR%\pilots.res" kernel32.lib user32.lib gdi32.lib winmm.lib SDL2.lib
if errorlevel 1 goto :fail

copy /y "%SDL2%\lib\%ARCH%\SDL2.dll" "%OUTDIR%\\" >nul

echo SUCCESS: %OUTDIR%\%EXENAME%
popd
exit /b 0

:fail
echo BUILD FAILED
popd
exit /b 1
