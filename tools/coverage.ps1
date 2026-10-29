# Line coverage via OpenCppCoverage (target: 100% on logic modules)
param([string]$Config = "Debug")
$root = Split-Path -Parent $PSScriptRoot
$occ = Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue
if (-not $occ) {
  $cand = @("$env:LOCALAPPDATA\OpenCppCoverage\OpenCppCoverage.exe",
            "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe")
  foreach ($c in $cand) { if (Test-Path $c) { $occ = $c; break } }
}
if (-not $occ) {
  Write-Warning "OpenCppCoverage not found. Install: winget install OpenCppCoverage.OpenCppCoverage"
  exit 3
}
& powershell -NoProfile -File "$root/tools/build.ps1" -Config $Config *> $null
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }
$exe = "$root/build/$($Config.ToLower())/tests/phantombt-tests.exe"
New-Item -ItemType Directory -Force -Path "$root/integration-results/coverage" | Out-Null
$rootWin = $root -replace '/', '\'
& $occ --sources "$rootWin\src" `
  --excluded_sources "$rootWin\src\gui" `
  --excluded_sources "$rootWin\src\probes" `
  --excluded_sources "$rootWin\src\cli" `
  --export_type html:"$rootWin\integration-results\coverage" `
  -- "$rootWin\build\debug\tests\phantombt-tests.exe"
exit $LASTEXITCODE
