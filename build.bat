@echo off
rem ====================================================================
rem  phantom-bt build script (equivalent of tools\build.ps1)
rem  usage:
rem     build.bat            -> Release build (default, build\release)
rem     build.bat release    -> Release build
rem     build.bat debug      -> Debug build   (build\debug)
rem     build.bat test       -> Debug build + run unit tests
rem     build.bat clean      -> remove build\ directory
rem ====================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set CMD=%1
if "%CMD%"=="" set CMD=release

if /i "%CMD%"=="clean" (
  if exist "%ROOT%build" rmdir /s /q "%ROOT%build"
  echo [ok] build\ removed
  exit /b 0
)

set CONFIG_PRESET=msvc-debug
set BUILD_PRESET=debug
if /i "%CMD%"=="release" set CONFIG_PRESET=msvc-release
if /i "%CMD%"=="release" set BUILD_PRESET=release
if /i "%CMD%"=="rel"     set CONFIG_PRESET=msvc-release
if /i "%CMD%"=="rel"     set BUILD_PRESET=release
if /i "%CMD%"=="test"    set CONFIG_PRESET=msvc-debug
if /i "%CMD%"=="test"    set BUILD_PRESET=debug

rem ---- locate Visual Studio via vswhere ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] vswhere.exe not found. Install Visual Studio with the C++ workload.
  exit /b 1
)
set VSPATH=
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
  echo [error] no Visual Studio installation found.
  exit /b 1
)
if not exist "%VSPATH%\Common7\Tools\VsDevCmd.bat" (
  echo [error] VsDevCmd.bat not found under "%VSPATH%".
  exit /b 1
)

rem ---- MSVC environment + cmake + ninja ----
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo >nul
if errorlevel 1 (
  echo [error] VsDevCmd failed.
  exit /b 1
)

cmake --preset %CONFIG_PRESET%
if errorlevel 1 (
  echo [error] cmake configure failed.
  exit /b 1
)

cmake --build --preset %BUILD_PRESET%
if errorlevel 1 (
  echo [error] build failed.
  exit /b 1
)

if /i "%CMD%"=="test" (
  "%ROOT%build\debug\tests\phantombt-tests.exe"
  if errorlevel 1 (
    echo [error] tests FAILED.
    exit /b 1
  )
)

echo [ok] build succeeded (%BUILD_PRESET%): build\%BUILD_PRESET%\
exit /b 0
