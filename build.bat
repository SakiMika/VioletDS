@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "STAMP="
for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss" 2^>nul') do set "STAMP=%%I"
if not defined STAMP set "STAMP=%RANDOM%"
set "TMPLOG=%TEMP%\RV_build_%RANDOM%_%RANDOM%.log"
call :BUILD_CORE >"%TMPLOG%" 2>&1
set "RC=%ERRORLEVEL%"
type "%TMPLOG%"
copy /y "%TMPLOG%" "%~dp0last_build.log" >nul
copy /y "%TMPLOG%" "%~dp0last_build(%STAMP%).log" >nul
del /q "%TMPLOG%" >nul 2>&1
if "%RC%"=="0" (echo [BUILD RESULT] SUCCESS) else (echo [BUILD RESULT] FAILED - errorlevel %RC%)
exit /b %RC%

:BUILD_CORE
if "%DEVKITPRO%"=="" set "DEVKITPRO=C:\devkitPro"
if "%DEVKITARM%"=="" set "DEVKITARM=%DEVKITPRO%\devkitARM"
set "PATH=%DEVKITARM%\bin;%DEVKITPRO%\tools\bin;%PATH%"

echo ================================================================
echo Ragnarok Violet NDS - single build
echo ================================================================
echo [TIME] %DATE% %TIME%
echo [SAVE] 128 KiB cartridge EEPROM / normal .sav
echo [DEVKITPRO] %DEVKITPRO%
echo [DEVKITARM] %DEVKITARM%
echo.

where make >nul 2>&1 || (echo [ERROR] make not found& exit /b 10)
where arm-none-eabi-g++ >nul 2>&1 || (echo [ERROR] devkitARM not found& exit /b 11)
where python >nul 2>&1 || (echo [ERROR] Python not found& exit /b 12)

echo [1/2] static test
python tests\ragnarok_firstpass_test.py
if errorlevel 1 exit /b %ERRORLEVEL%

echo.
echo [2/2] make clean + make -j2
make clean
if errorlevel 1 exit /b %ERRORLEVEL%
make -j2
if errorlevel 1 exit /b %ERRORLEVEL%

if not exist "Ragnarok Violet.nds" (
    echo [ERROR] Build succeeded but Ragnarok Violet.nds was not created
    exit /b 20
)
if exist "Ragnarok_Violet.nds" (
    echo [ERROR] Internal build artifact Ragnarok_Violet.nds was not cleaned up
    exit /b 21
)

for %%F in ("Ragnarok Violet.nds") do echo [ROM] %%~fF  [%%~zF bytes]
echo [SAVE] melonDS: 128 KiB ROM-name .sav after first EEPROM write.
echo [SUCCESS] Ragnarok Violet.nds ready. DSi runs this normal NDS build directly.
exit /b 0
