@echo off
REM Run tests from an agent's isolated build dir. Usage: test_agent.bat <name> [-R regex]
setlocal
chcp 65001 >nul
set BDIR=out\build\agent-%1
shift
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
ctest --test-dir %BDIR% --output-on-failure %1 %2 %3
endlocal
