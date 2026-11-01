# Coverage gate: build Debug -> OpenCppCoverage (cobertura) -> cov_gaps.py gate.
# Fails (nonzero exit) when overall src/ line coverage is below $ThresholdPct.
param([string]$Config = "Debug")

# ---------------------------------------------------------------------------
# Gate threshold. 96.1% = floor(96.17%, 0.1%). Source: local full run of
# tools/coverage.ps1 (cobertura export) on 2026-11-01 --
# integration-results/coverage.xml: 3342/3475 lines = 96.17% line coverage
# over src/ (gui/probes/cli excluded). Ratchet only upward, never lower it.
$ThresholdPct = 96.1
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# 1) build (same wrapper as tools/run-tests.ps1: VsDevCmd + cmake preset)
& powershell -NoProfile -File "$root/tools/build.ps1" -Config $Config
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

$exe = "$root/build/$($Config.ToLower())/tests/phantombt-tests.exe"
if (-not (Test-Path $exe)) { Write-Error "test exe not found: $exe"; exit 1 }

# 2) locate OpenCppCoverage (same probe order as tools/coverage.ps1)
$occ = Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue
if (-not $occ) {
  $cand = @("$env:LOCALAPPDATA\OpenCppCoverage\OpenCppCoverage.exe",
            "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe")
  foreach ($c in $cand) { if (Test-Path $c) { $occ = $c; break } }
}
if (-not $occ) {
  Write-Error "OpenCppCoverage not found. Install: choco install opencppcoverage (CI) / winget install OpenCppCoverage.OpenCppCoverage"
  exit 3
}

# 3) measure, exporting cobertura for the gate parser.
#    _NO_DEBUG_HEAP=1 disables Debug-CRT heap validation: it collides with the
#    coverage instrumentation and sporadically asserts during process teardown.
$env:_NO_DEBUG_HEAP = "1"
$rootWin = $root -replace '/', '\'
New-Item -ItemType Directory -Force -Path "$root/integration-results" | Out-Null
$xmlOut = "$rootWin\integration-results\coverage.xml"
if (Test-Path $xmlOut) { Remove-Item $xmlOut -Force }
& $occ --sources "$rootWin\src" `
  --excluded_sources "$rootWin\src\gui" `
  --excluded_sources "$rootWin\src\probes" `
  --excluded_sources "$rootWin\src\cli" `
  --export_type cobertura:"$xmlOut" `
  -- "$rootWin\build\$($Config.ToLower())\tests\phantombt-tests.exe"
$covExit = $LASTEXITCODE
Write-Output "OpenCppCoverage exit code: $covExit (3 = covered program exited abnormally, report still written)"
# 0 = clean run, 3 = target crashed/abnormal exit but the report was produced.
# Anything else = measurement itself failed.
if ($covExit -ne 0 -and $covExit -ne 3) { Write-Error "OpenCppCoverage failed (exit $covExit)"; exit 1 }
if (-not (Test-Path $xmlOut)) { Write-Error "coverage report not produced: $xmlOut"; exit 1 }

# 4) parse + gate (nonzero exit + failing-file list comes from cov_gaps.py)
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
  Write-Error "python not found on PATH (required by tools/cov_gaps.py)"
  exit 1
}
& python "$root/tools/cov_gaps.py" --xml "$xmlOut" --threshold $ThresholdPct
exit $LASTEXITCODE
