@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Build the Windows installer with Inno Setup. Run after a normal build
REM (scripts\build-and-test.cmd) so build\SQLTerminal.exe and its DLLs exist.
setlocal EnableExtensions

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo ERROR: Inno Setup not found. Install it: winget install JRSoftware.InnoSetup
  exit /b 1
)

"%ISCC%" "%~dp0..\packaging\installer.iss" || exit /b 1
echo.
echo Installer written to build\installer\
