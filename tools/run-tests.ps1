# One-shot: build + unit tests + all loopback integrations
param([string]$Config = "Release")
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
& powershell -NoProfile -File "$root/tools/build.ps1" -Config $Config *> $null
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }
$exe = "$root/build/$($Config.ToLower())/tests/phantombt-tests.exe"
& $exe
$unit = $LASTEXITCODE
$integ = 0
foreach ($m in @("m0", "m1", "m2", "m3")) {
  & powershell -NoProfile -File "$root/tools/run-$m.ps1" -SkipRealDevice *> $null
  $rc = $LASTEXITCODE
  if ($rc -ne 0 -and $rc -ne 3) { $integ = 1 }  # 3 = 真机段 SKIP（预期）
}
if ($unit -ne 0 -or $integ -ne 0) { exit 1 }
Write-Output "ALL GREEN (unit + M0..M3 loopback)"
exit 0
