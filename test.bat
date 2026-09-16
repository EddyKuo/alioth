@echo off
REM Run the Alioth test suite. Usage: test.bat [preset] [-R <regex>]
setlocal
chcp 65001 >nul
set PRESET=%1
if "%PRESET%"=="" set PRESET=windows-x64-debug
shift

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
ctest --preset %PRESET% %1 %2 %3
exit /b %errorlevel%
