@echo off
rem Per-file x64 warning check for the 64-bit migration (LP64 pointer-in-int).
rem Usage: scripts\check_x64_file.bat render.c [more.c ...]
rem Compiles the given SRC files with MSVC x64, same flags/suppressions as the
rem migration probe (pointer-mismatch warnings stay VISIBLE: they are the
rem signal). Prints raw cl output to stdout; exit code = cl exit code.
setlocal
set VCV=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCV%" (echo NO_VCVARS& exit /b 2)
call "%VCV%" x64 >nul 2>&1
if errorlevel 1 (echo VCVARS_FAIL& exit /b 3)
pushd "%~dp0\..\SRC"
set SDL2=..\third_party\SDL2
set ADLMIDI=..\third_party\libADLMIDI
set INCLUDE=%INCLUDE%;%SDL2%\include;%ADLMIDI%\include
cl /c /TC /W3 /WX- /Od /DNDEBUG /DWIN32 /MT /wd4996 /wd4113 /wd4028 /wd4029 /wd4090 /wd4244 /wd4267 /wd4554 /wd4838 /wd4309 /wd4005 /wd4701 %*
set ec=%ERRORLEVEL%
for %%f in (%*) do del /Q "%%~nf.obj" 2>nul
popd
exit /b %ec%
