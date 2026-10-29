@echo off
rem ====================================================================
rem  Create the GitHub remote repository and push (personal-use open
rem  source, PolyForm Noncommercial; commercial use requires license).
rem
rem  Prerequisites:
rem    1. gh CLI installed and logged in:  gh auth login
rem    2. network access to github.com (set HTTP(S)_PROXY if needed)
rem
rem  usage: tools\create-repo.bat [repo-name]
rem         default repo name = phantom-bt
rem ====================================================================
setlocal
set REPO=%1
if "%REPO%"=="" set REPO=phantom-bt
set ROOT=%~dp0..

where gh >nul 2>nul
if errorlevel 1 (
  echo [error] GitHub CLI not found. Install: winget install GitHub.cli  then: gh auth login
  exit /b 1
)

gh auth status >nul 2>nul
if errorlevel 1 (
  echo [error] gh not authenticated. Run: gh auth login
  exit /b 1
)

echo Creating public repo %REPO% ...
gh repo create %REPO% --public --description "Windows virtual USB HID / Bluetooth / BLE HID simulation testbed (WinRT-visible) - personal use free, commercial license required" --source "%ROOT%" --remote origin --push
if errorlevel 1 (
  echo [error] repo create or push failed. Check network/proxy and gh auth.
  exit /b 1
)

echo.
echo Done. Remote: https://github.com/%REPO%
echo License: PolyForm Noncommercial 1.0.0 (see LICENSE - commercial use requires a separate license)
exit /b 0
