@echo off
call "%~dp0_idf-build.bat" academy
if errorlevel 1 exit /b 1
call "%~dp0flash.bat" %1
