@echo off
setlocal

set "PORT=%~1"
if "%PORT%"=="" set "PORT=%FLASH_PORT%"
if "%PORT%"=="" (
    echo Usage: flash.bat COMx
    echo   or set FLASH_PORT=COMx
    exit /b 1
)

call "%~dp0env.bat"
cd /d "%~dp0"

idf.py -p %PORT% flash monitor
