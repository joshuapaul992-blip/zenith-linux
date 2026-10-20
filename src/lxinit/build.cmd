@echo off
rem ===========================================================================
rem build.cmd -- build lxinit.exe (native NT subsystem, x64) with MSVC/DDK.
rem
rem Requires an x64 toolchain whose INCLUDE/LIB point at ntdll.lib, e.g. the
rem Windows Server 2003 R2 DDK (build env for NT 5.2) or a WDK x64 prompt.
rem Run from a configured "x64 Checked/Free" or Visual Studio x64 shell.
rem
rem Usage:  build.cmd            (free/optimized)
rem         build.cmd debug      (no optimization, full symbols)
rem ===========================================================================
setlocal

set SRC=lxinit.c
set OUT=lxinit.exe
set OBJ=lxinit.obj

if /i "%~1"=="debug" (
    set CFLAGS=/c /GS- /Od /Zi /Zl /W4 /nologo
    set LFLAGS=/DEBUG
) else (
    set CFLAGS=/c /GS- /O2 /Zl /W4 /nologo
    set LFLAGS=
)

echo [lxinit] compiling %SRC%
cl %CFLAGS% %SRC%
if errorlevel 1 goto :fail

echo [lxinit] linking %OUT% (/SUBSYSTEM:NATIVE,5.02)
link /nologo /SUBSYSTEM:NATIVE,5.02 /ENTRY:NtProcessStartup /NODEFAULTLIB ^
     /MACHINE:X64 %LFLAGS% %OBJ% ntdll.lib /OUT:%OUT%
if errorlevel 1 goto :fail

echo [lxinit] built %OUT%
endlocal & exit /b 0

:fail
echo [lxinit] BUILD FAILED
endlocal & exit /b 1
