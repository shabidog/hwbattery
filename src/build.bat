@echo off
REM ============================================================
REM  Build the Huawei battery tray tool
REM
REM  Requires : mingw-w64 gcc + windres on PATH
REM  Output   : ..\release\hwbattery.exe  (GUI subsystem, no console)
REM
REM  Keep this file ASCII-only: cmd.exe parses .bat using the OEM
REM  code page (GBK on zh-CN Windows), so UTF-8 Chinese text here
REM  corrupts quoting and the script fails to run. hwbattery.rc is
REM  Chinese, but it is read by windres as UTF-8 - unrelated concern.
REM ============================================================
setlocal
cd /d "%~dp0"

where gcc >nul 2>nul
if errorlevel 1 (
    echo [!] gcc not found - make sure mingw-w64 is on PATH.
    exit /b 1
)
where windres >nul 2>nul
if errorlevel 1 (
    echo [!] windres not found - make sure mingw-w64 is on PATH.
    exit /b 1
)

set "OUTDIR=%~dp0..\release"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo Compiling resources - version info, manifest, icon ...
windres --codepage=65001 -O coff hwbattery.rc -o hwbattery_res.o
if errorlevel 1 (
    echo.
    echo [!] resource compile failed
    exit /b 1
)

echo Building to "%OUTDIR%\hwbattery.exe" ...
gcc -O2 -Wall -Wextra -o "%OUTDIR%\hwbattery.exe" hwbattery.c hwbattery_res.o ^
    -lsetupapi -lole32 -loleaut32 -luuid -lwbemuuid ^
    -lshell32 -luser32 -ladvapi32 -lgdi32 -mwindows

if errorlevel 1 (
    echo.
    echo [!] build failed
    exit /b 1
)

del /q hwbattery_res.o >nul 2>nul

echo.
echo [OK] built "%OUTDIR%\hwbattery.exe"
echo      Setting the charge threshold requires running as administrator.
endlocal
