@echo off
rem Build lxrun.exe -- user-mode ELF loader + syscall emulator (x64).
rem Run from an x64 MSVC command prompt (Visual Studio "x64 Native Tools").
rem
rem /TP compiles as C++ on purpose: lxrun.c uses C99 (inline `for (int i..)`,
rem mid-block declarations) that the older MSVC *C* compiler (VS2012 and
rem earlier) rejects. The code is C++-clean, so /TP builds everywhere from
rem VS2012 through current. NOTE: however it is built, lxrun must RUN on
rem Windows 10 1607+/11 x64 (it needs user-mode FSGSBASE).
rem
rem Usage:  build.cmd            (optimized)
rem         build.cmd debug      (symbols, no optimization)
setlocal

if /i "%~1"=="debug" (set CFLAGS=/TP /Zi /Od /W4 /nologo) else (set CFLAGS=/TP /O2 /W4 /nologo)

echo [lxrun] compiling lxrun.c (as C++)
cl %CFLAGS% lxrun.c /link /OUT:lxrun.exe
if errorlevel 1 (echo [lxrun] BUILD FAILED & endlocal & exit /b 1)
echo [lxrun] built lxrun.exe
endlocal
