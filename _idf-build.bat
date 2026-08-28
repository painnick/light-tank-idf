@echo off
setlocal EnableDelayedExpansion

set "ROOT=%~dp0"
set "PROFILE=%~1"
if "%PROFILE%"=="" set "PROFILE=light"

call "%ROOT%env.bat"
cd /d "%ROOT%"

if /i "%PROFILE%"=="academy" (
    set "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.academy"
    set "PROFILE_LABEL=Academy 2호"
) else (
    set "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c6"
    set "PROFILE_LABEL=Light Tank"
)

set "LAST="
if exist "%ROOT%.build-profile" set /p LAST=<"%ROOT%.build-profile"

if /i not "!LAST!"=="%PROFILE%" (
    echo [build] 프로필: !PROFILE_LABEL! ^(sdkconfig 재생성^)
    if exist sdkconfig del sdkconfig
    echo %PROFILE%> "%ROOT%.build-profile"
    idf.py set-target esp32c6
    if errorlevel 1 exit /b 1
) else (
    idf.py reconfigure
    if errorlevel 1 exit /b 1
)

idf.py build
exit /b %ERRORLEVEL%
