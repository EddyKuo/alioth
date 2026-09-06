@echo off
REM Isolated build for parallel agents: each agent gets its own binary dir so
REM concurrent ninja runs cannot clobber each other.
REM Usage: build_agent.bat <name> [target]
setlocal
chcp 65001 >nul
if "%1"=="" (
    echo [ERROR] usage: build_agent.bat ^<name^> [target]
    exit /b 1
)
set BDIR=out\build\agent-%1

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul

cmake -S D:\code\Alioth -B %BDIR% -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
      -DCMAKE_PREFIX_PATH=C:/Qt/6.8.1/msvc2022_64 || exit /b 1
if "%2"=="" (
    cmake --build %BDIR% || exit /b 1
) else (
    cmake --build %BDIR% --target %2 || exit /b 1
)
echo [OK] %BDIR%
endlocal
