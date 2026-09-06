@echo off
REM Alioth build script. CMake needs the MSVC environment, so call vcvars64 first.
REM Usage: build.bat [preset] [target]   default preset: windows-x64-debug
REM A target narrows the build to one thing. That matters when several agents
REM edit the tree at once: an unrelated half-written file must not stop you from
REM building and testing your own.
REM ASCII-only on purpose: the console codepage mangles UTF-8 in .bat files.

setlocal
chcp 65001 >nul
set PRESET=%1
if "%PRESET%"=="" set PRESET=windows-x64-debug
set TARGET=%2

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] MSVC environment not found
    exit /b 1
)

cmake --preset %PRESET% || exit /b 1
if "%TARGET%"=="" (
    cmake --build --preset %PRESET% || exit /b 1
) else (
    cmake --build --preset %PRESET% --target %TARGET% || exit /b 1
)

echo [OK] build finished: out\build\%PRESET%
endlocal
