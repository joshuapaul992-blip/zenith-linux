@echo off
rem Build lxrun.exe -- user-mode static-ELF loader + syscall emulator (x64).
rem Run from an x64 MSVC command prompt (Visual Studio "x64 Native Tools").
rem Usage:  build.cmd            (optimized)
rem         build.cmd debug      (symbols, no optimization)
setlocal

if /i "%~1"=="debug" (set CFLAGS=/Zi /Od /W4 /nologo) else (set CFLAGS=/O2 /W4 /nologo)

echo [lxrun] compiling lxrun.c
cl %CFLAGS% lxrun.c /link /OUT:lxrun.exe
if errorlevel 1 (echo [lxrun] BUILD FAILED & endlocal & exit /b 1)
echo [lxrun] built lxrun.exe
endlocal
