param([string]$Config = "Debug")
$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere not found"; exit 1 }
$vsPath = & $vswhere -latest -property installationPath
$devCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $devCmd)) { Write-Error "VsDevCmd not found at $devCmd"; exit 1 }
$preset = $Config.ToLower()
cmd /c "`"$devCmd`" -arch=x64 -no_logo && cmake --preset msvc-$preset && cmake --build --preset $preset"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
