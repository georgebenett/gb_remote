@echo off
setlocal

set "PORT=%~1"
set "FIRMWARE_DIR=%~dp0"
set "BUILD_DIR=%FIRMWARE_DIR%build"

if not exist "%BUILD_DIR%\flash_args" (
    echo ERROR: %BUILD_DIR%\flash_args not found.
    echo Build the firmware first in WSL or ESP-IDF.
    echo Expected build output under:
    echo   %BUILD_DIR%
    exit /b 1
)

if "%PORT%"=="" goto :usage

set "PYEXE=py"
where py >nul 2>nul
if errorlevel 1 set "PYEXE=python"

%PYEXE% -c "import esptool" >nul 2>nul
if errorlevel 1 goto :missing_esptool

pushd "%BUILD_DIR%"
echo Flashing to %PORT% ...
%PYEXE% -m esptool --chip esp32s3 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash @flash_args
set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" (
    echo.
    echo Flash failed.
    echo If auto-reset does not work, put the board in bootloader mode manually:
    echo   1. Hold BOOT
    echo   2. Tap RESET
    echo   3. Release RESET
    echo   4. Release BOOT
    exit /b %RC%
)

echo.
echo Flash complete.
exit /b 0

:missing_esptool
echo ERROR: Python module "esptool" is not installed for %PYEXE%.
echo.
echo Install it with one of these commands in Windows cmd:
echo   py -m pip install --user esptool
echo or
echo   python -m pip install --user esptool
echo.
echo Then verify with:
echo   %PYEXE% -m esptool version
echo.
echo After that, run again:
echo   %~n0 %PORT%
exit /b 1

:usage
echo Usage:
  %~n0 COMx

echo Example:
  %~n0 COM5

echo.
echo Available serial ports:
powershell -NoProfile -Command "Get-CimInstance Win32_SerialPort ^| Select-Object DeviceID,Name ^| Format-Table -AutoSize"
exit /b 1
