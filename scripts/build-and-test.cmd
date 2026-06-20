@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Configure, build, and run the SqlCore golden tests with the VS 2026 toolchain.
REM Usage:  scripts\build-and-test.cmd
setlocal EnableExtensions

set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
  echo ERROR: Visual Studio 2026 not found at "%VSROOT%".
  exit /b 1
)

REM Put the VS-bundled CMake/Ninja and the VS Installer (for vswhere) on PATH,
REM then bring in the MSVC env.
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build"

cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Debug || exit /b 1
cmake --build "%BUILD%" || exit /b 1
ctest --test-dir "%BUILD%" --output-on-failure
exit /b %ERRORLEVEL%
