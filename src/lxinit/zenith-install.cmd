@echo off
rem ===========================================================================
rem zenith-install.cmd -- safely register lxinit as a BootExecute program.
rem
rem Unlike zenith-install.reg, this APPENDS "lxinit" to the existing
rem BootExecute REG_MULTI_SZ value, preserving autochk and any third-party
rem entries already present. Run elevated (Administrator).
rem
rem Usage:  zenith-install.cmd [RootfsPath] [EnableX11]
rem   RootfsPath  DOS path to the rootfs root   (default C:\rootfs)
rem   EnableX11   0 or 1                         (default 0)
rem ===========================================================================
setlocal enabledelayedexpansion

set "SM=HKLM\SYSTEM\CurrentControlSet\Control\Session Manager"
set "ROOTFS=%~1"
if "%ROOTFS%"=="" set "ROOTFS=C:\rootfs"
set "X11=%~2"
if "%X11%"=="" set "X11=0"

rem --- install the binary ---------------------------------------------------
if exist "%~dp0lxinit.exe" (
    echo [install] copying lxinit.exe to %SystemRoot%\System32
    copy /y "%~dp0lxinit.exe" "%SystemRoot%\System32\lxinit.exe" >nul
) else (
    echo [install] NOTE lxinit.exe not found next to this script; build it first.
)

rem --- append lxinit to BootExecute (idempotent) ----------------------------
reg query "%SM%" /v BootExecute 2>nul | find /i "lxinit" >nul
if %errorlevel%==0 (
    echo [install] BootExecute already contains lxinit
) else (
    set "CUR="
    for /f "tokens=2,*" %%a in ('reg query "%SM%" /v BootExecute ^| find /i "REG_MULTI_SZ"') do set "CUR=%%b"
    if defined CUR (
        set "NEW=!CUR!\0lxinit"
    ) else (
        set "NEW=autocheck autochk *\0lxinit"
    )
    echo [install] setting BootExecute = !NEW!
    reg add "%SM%" /v BootExecute /t REG_MULTI_SZ /d "!NEW!" /f >nul
)

rem --- configuration key ----------------------------------------------------
echo [install] writing Zenith config (RootfsPath=%ROOTFS% EnableX11=%X11%)
reg add "%SM%\Zenith" /v RootfsPath /t REG_SZ    /d "%ROOTFS%" /f >nul
reg add "%SM%\Zenith" /v EnableX11  /t REG_DWORD  /d %X11%      /f >nul

echo [install] done. Reboot to run lxinit at BootExecute.
endlocal
