@echo off
REM ASCII only. Console codepage turns UTF-8 comments into garbage and breaks the syntax.
REM
REM Fetch the official qpdf Windows binary release into third_party/qpdf/.
REM Rationale for using the official release instead of the vcpkg port is in
REM third_party/qpdf/README.md.
REM
REM Usage: tools\qpdf\fetch_qpdf.bat [version]

setlocal enabledelayedexpansion

set QPDF_VERSION=%1
if "%QPDF_VERSION%"=="" set QPDF_VERSION=12.4.1

set ROOT=%~dp0..\..
set DEST=%ROOT%\third_party\qpdf
set ARCHIVE=%TEMP%\qpdf-%QPDF_VERSION%-msvc64.zip
set URL=https://github.com/qpdf/qpdf/releases/download/v%QPDF_VERSION%/qpdf-%QPDF_VERSION%-msvc64.zip

if exist "%DEST%\bin\qpdf.exe" (
    echo qpdf already present: %DEST%\bin\qpdf.exe
    "%DEST%\bin\qpdf.exe" --version
    exit /b 0
)

echo Downloading %URL%
curl -sSL -o "%ARCHIVE%" "%URL%"
if errorlevel 1 (
    echo Download failed.
    exit /b 1
)

set STAGE=%TEMP%\qpdf-stage-%QPDF_VERSION%
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"

powershell -NoProfile -Command "Expand-Archive -LiteralPath '%ARCHIVE%' -DestinationPath '%STAGE%' -Force"
if errorlevel 1 (
    echo Extract failed.
    exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%"
REM Only bin/ is needed. The library, headers and docs would add ~19 MB for no benefit:
REM we shell out to qpdf.exe rather than linking against libqpdf, which keeps the
REM engine-level dependency count at the three named in PRD section 4.1.
xcopy /e /i /y /q "%STAGE%\qpdf-%QPDF_VERSION%-msvc64\bin" "%DEST%\bin" >nul
if errorlevel 1 (
    echo Copy failed.
    exit /b 1
)

del "%ARCHIVE%" >nul 2>&1
rmdir /s /q "%STAGE%" >nul 2>&1

echo Installed to %DEST%\bin
"%DEST%\bin\qpdf.exe" --version
endlocal
