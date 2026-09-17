@echo off
REM Alioth CI gate. Runs the same checks a pull request must pass.
REM Usage: ci.bat [preset]   default: windows-x64-release
REM ASCII-only on purpose: the console codepage mangles UTF-8 in .bat files.

setlocal
chcp 65001 >nul
set PRESET=%1
if "%PRESET%"=="" set PRESET=windows-x64-release
set BDIR=out\build\%PRESET%

echo [1/6] configure and build
call "D:\code\Alioth\build.bat" %PRESET% || goto :fail

echo [2/6] tests
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
ctest --test-dir %BDIR% --output-on-failure || goto :fail

echo [3/6] performance regression gate
REM PRD section 9 defines the gate as "block on >10%% regression against the baseline",
REM not "block on absolute budget". Budget violations are still printed loudly.
REM The known full-text search overshoot is tracked in
REM exceptions/EXC_20260905_RD_SA_parallel_search.md - gating on it would leave CI
REM permanently red, and a permanently red gate hides real regressions.
REM Search timing is measured as the median of 3 runs: its natural spread is about
REM 20%% single-shot, and a 10%% gate on a 20%% metric fires spuriously - which teaches
REM people to ignore the gate, hiding the regressions it exists to catch.
REM The baseline is per preset: Debug and Release numbers differ by several times,
REM and comparing across them would produce noise that looks like regressions.
REM Refresh deliberately with --update-baseline after an intended change.
"%BDIR%\alioth_bench.exe" --generate 500 --steps 60 --baseline tests\perf_baseline_%PRESET%.json || goto :fail

echo [4/6] traceability matrix is current
python tools\traceability.py || goto :fail

echo [5/6] layering check
REM Catches PDFium headers outside the engine layer, Qt in the domain layer, and
REM #ifdef _WIN32 outside the platform layer. CMake target deps only catch link
REM errors; a PUBLIC dependency can let a layering violation compile cleanly.
python tools\check_layering.py || goto :fail

echo [6/6] sprint metrics
REM Test counts and requirement stats are generated, never hand-written.
REM sprint/current/status.md once claimed both "13 test targets green" and
REM "24 test targets green" in the same file while the real number was in the
REM hundreds - a status document with wrong numbers cannot be used for
REM scheduling or as a release gate, and it causes duplicated work.
python tools\sprint_status.py %PRESET% || goto :fail

echo.
echo [CI OK] %PRESET%
endlocal
exit /b 0

:fail
echo.
echo [CI FAILED] see output above
endlocal
exit /b 1
