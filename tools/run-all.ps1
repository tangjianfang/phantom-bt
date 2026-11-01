# tools/run-all.ps1 -- one-command full flow over every gate this repo has:
#   01 build-release     tools/build.ps1 -Config Release                 (blocking)
#   02 unit-tests        Debug build + build/debug/tests/phantombt-tests.exe (blocking)
#   03 coverage-gate     tools/coverage-gate.ps1                         (blocking)
#   04 loopback-suite    tools/run-tests.ps1 (build + unit + M0..M3 loopback) (blocking)
#   05 check-tools       tools/check-tools.ps1 (SKIPPED if file absent)  (blocking when present)
#   06 gui-verify        tools/gui-verify.ps1 (real-path GUI clicks)     (blocking)
#   07 m0-realdevice     tools/run-m0.ps1  (requires realdevice=PASS)    (blocking)
#   08 m1-realdevice     tools/run-m1.ps1  (registry caliber PASS, i.e. PASS or PASS(registry)) (blocking)
#   09 m2/m3-realdevice  tools/run-m2.ps1 + tools/run-m3.ps1             (recorded only, NOT blocking)
# Per step one "[STAGE] <name>: PASS/FAIL/SKIPPED" line is printed; at the end
# a summary table goes to the console and integration-results/full-test-report.md
# (per-stage seconds + the tail of every failed stage; full logs live in
# integration-results/full-test-logs/NN-*.log). Exit 0 = every blocking stage
# PASSed (SKIPPED never fails); exit 1 = at least one blocking stage FAILed.
#
# Ports: every serve/usbip port is $Port (default 3425). The usbip default 3240
# is held by another resident project (uacsimd) on this machine -- uacsimd is
# NEVER killed by this script; the only process ever taskkilled is phantombt.exe.
# Real-device pre-clean (once, right before stage 07): taskkill /IM
# phantombt.exe /F (failure ignored) + "C:\Program Files\USBip\usbip.exe"
# detach -a (failure ignored). Admin terminal assumed (kernel attach legs);
# missing verify tools (python / OpenCppCoverage) are installed silently first
# (winget --silent, then choco -y) -- drivers are NEVER installed silently.
# -ListStages prints the plan without running anything.
param([int]$Port = 3425, [switch]$ListStages)

if ($ListStages) {
  Write-Output '01 build-release      tools/build.ps1 -Config Release                    (blocking)'
  Write-Output '02 unit-tests         Debug build + build/debug/tests/phantombt-tests.exe (blocking)'
  Write-Output '03 coverage-gate      tools/coverage-gate.ps1                            (blocking)'
  Write-Output '04 loopback-suite     tools/run-tests.ps1                                 (blocking)'
  Write-Output '05 check-tools        tools/check-tools.ps1  (SKIPPED when file absent)   (blocking)'
  Write-Output '06 gui-verify         tools/gui-verify.ps1 -Port <port>                   (blocking)'
  Write-Output '07 m0-realdevice      tools/run-m0.ps1 -Port <port>  realdevice=PASS      (blocking)'
  Write-Output '08 m1-realdevice      tools/run-m1.ps1 -Port <port>  registry caliber     (blocking)'
  Write-Output '09 m2/m3-realdevice   tools/run-m2.ps1 + run-m3.ps1 -Port <port>          (record only)'
  exit 0
}

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$resDir = Join-Path $root 'integration-results'
$logDir = Join-Path $resDir 'full-test-logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$t0 = Get-Date

$script:stages = New-Object System.Collections.Generic.List[object]

function Write-Stage([string]$name, [bool]$blocking, [string]$status, [double]$sec, [string]$detail, [string]$log) {
  $script:stages.Add([pscustomobject]@{ Name = $name; Blocking = $blocking; Status = $status; Sec = [math]::Round($sec, 1); Detail = $detail; Log = $log })
  $line = '[STAGE] ' + $name + ': ' + $status
  if ($detail) { $line = $line + '  [' + $detail + ']' }
  Write-Output $line
}

function Get-LogTail([string]$log, [int]$last = 30) {
  if (-not $log -or -not (Test-Path $log)) { return '' }
  $t = @((Get-Content $log -ErrorAction SilentlyContinue) | Where-Object { $_ -ne '' })
  if ($t.Count -eq 0) { return '' }
  $n = [math]::Min($last, $t.Count)
  return ($t[($t.Count - $n)..($t.Count - 1)] -join "`n")
}

# Run one child .ps1 into its stage log and classify by exit code:
# 0 = PASS, anything in $skipCodes = SKIPPED (repo convention: 3 = env SKIP),
# everything else = FAIL. Returns the status string.
function Invoke-ScriptStage([string]$name, [bool]$blocking, [string]$logName,
                             [string]$script, [string[]]$scriptArgs = @(), [int[]]$skipCodes = @()) {
  $log = Join-Path $logDir $logName
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  & powershell -NoProfile -File $script @scriptArgs *> $log
  $rc = $LASTEXITCODE
  $sw.Stop()
  $status = 'FAIL'; $detail = 'exit ' + $rc
  if ($rc -eq 0) { $status = 'PASS'; $detail = '' }
  elseif ($skipCodes -contains $rc) { $status = 'SKIPPED'; $detail = 'exit ' + $rc }
  Write-Stage $name $blocking $status $sw.Elapsed.TotalSeconds $detail $log
  return $status
}

# ---- preflight: verify tools, silent winget/choco install when missing -------
function Test-OpenCppCoverage {
  if (Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue) { return $true }
  foreach ($p in @((Join-Path $env:LOCALAPPDATA 'OpenCppCoverage\OpenCppCoverage.exe'),
                   (Join-Path $env:ProgramFiles 'OpenCppCoverage\OpenCppCoverage.exe'))) {
    if (Test-Path $p) { return $true }
  }
  return $false
}
function Update-PathFromEnvironment {
  $env:Path = (@([Environment]::GetEnvironmentVariable('Path', 'Machine'),
                 [Environment]::GetEnvironmentVariable('Path', 'User')) -join ';')
}
function Install-ToolSilently([string]$wingetId, [string]$chocoId) {
  # winget first (silent), choco -y as fallback; True only on a clean install rc.
  $wg = Get-Command winget -ErrorAction SilentlyContinue
  if ($wg) {
    & winget install -e --id $wingetId --silent --accept-package-agreements --accept-source-agreements *> $null
    if ($LASTEXITCODE -eq 0) { return $true }
  }
  $ch = Get-Command choco -ErrorAction SilentlyContinue
  if ($ch) {
    & choco install $chocoId -y *> $null
    if ($LASTEXITCODE -eq 0) { return $true }
  }
  return $false
}

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$usbipExe = (Get-Command usbip.exe -ErrorAction SilentlyContinue).Source
if (-not $usbipExe -and (Test-Path 'C:\Program Files\USBip\usbip.exe')) { $usbipExe = 'C:\Program Files\USBip\usbip.exe' }
Write-Output ('== preflight: admin=' + $admin + '  port=' + $Port + '  usbip=' + $(if ($usbipExe) { $usbipExe } else { 'not found (real-device legs will SKIP)' }))
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
  Write-Output '== preflight: python missing -- installing silently (winget/choco)'
  [void](Install-ToolSilently 'Python.Python.3.12' 'python3')
  Update-PathFromEnvironment
  Write-Output ('== preflight: python now on PATH: ' + ($null -ne (Get-Command python -ErrorAction SilentlyContinue)))
}
if (-not (Test-OpenCppCoverage)) {
  Write-Output '== preflight: OpenCppCoverage missing -- installing silently (winget/choco)'
  [void](Install-ToolSilently 'OpenCppCoverage.OpenCppCoverage' 'opencppcoverage')
  Update-PathFromEnvironment
  Write-Output ('== preflight: OpenCppCoverage now found: ' + (Test-OpenCppCoverage))
}

# =========================================================== 01 build-release
Invoke-ScriptStage 'build-release' $true '01-build-release.log' (Join-Path $root 'tools\build.ps1') @('-Config', 'Release') | Out-Null

# =========================================================== 02 unit-tests
# The unit suite runs the Debug build (build/debug/tests/phantombt-tests.exe),
# so Debug is built first (incremental when already current).
$log = Join-Path $logDir '02-unit-tests.log'
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& powershell -NoProfile -File (Join-Path $root 'tools\build.ps1') -Config Debug *> $log
$rcDbg = $LASTEXITCODE
$status = 'FAIL'; $detail = 'debug build failed, exit ' + $rcDbg
if ($rcDbg -eq 0) {
  $exe = Join-Path $root 'build\debug\tests\phantombt-tests.exe'
  if (-not (Test-Path $exe)) {
    $detail = 'test exe not found: ' + $exe
  } else {
    & $exe *>> $log
    if ($LASTEXITCODE -eq 0) { $status = 'PASS'; $detail = '' }
    else { $detail = 'phantombt-tests.exe exit ' + $LASTEXITCODE }
  }
}
$sw.Stop()
Write-Stage 'unit-tests' $true $status $sw.Elapsed.TotalSeconds $detail $log

# =========================================================== 03 coverage-gate
# coverage-gate.ps1 rebuilds Debug itself and runs OpenCppCoverage + cov_gaps;
# rc 3 = OpenCppCoverage not installable (env not ready) -> SKIPPED, not FAIL.
Invoke-ScriptStage 'coverage-gate' $true '03-coverage-gate.log' (Join-Path $root 'tools\coverage-gate.ps1') @() @(3) | Out-Null

# =========================================================== 04 loopback-suite
Invoke-ScriptStage 'loopback-suite' $true '04-loopback-suite.log' (Join-Path $root 'tools\run-tests.ps1') @() | Out-Null

# =========================================================== 05 check-tools
$checkTools = Join-Path $root 'tools\check-tools.ps1'
if (-not (Test-Path $checkTools)) {
  Write-Stage 'check-tools' $true 'SKIPPED' 0.0 'tools/check-tools.ps1 not created yet' ''
} else {
  $log = Join-Path $logDir '05-check-tools.log'
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  & powershell -NoProfile -File $checkTools *> $log
  $rc = $LASTEXITCODE
  $sw.Stop()
  $status = 'FAIL'; $detail = 'exit ' + $rc
  if ($rc -eq 0) { $status = 'PASS'; $detail = '' }
  elseif ($rc -eq 3) { $status = 'SKIPPED'; $detail = 'environment not ready (exit 3)' }
  else {
    # rc 1 also fires for the audited known-defect rows that FAIL by design
    # until fixed (see $knownDefectIds in tools/check-tools.ps1).
    $fails = @((Get-Content $log -ErrorAction SilentlyContinue) | Where-Object { $_ -match '^FAIL ' }).Count
    $detail = 'exit ' + $rc + '; ' + $fails + ' FAIL row(s) (known-defect rows FAIL by design until fixed)'
  }
  Write-Stage 'check-tools' $true $status $sw.Elapsed.TotalSeconds $detail $log
}

# =========================================================== 06 gui-verify
Invoke-ScriptStage 'gui-verify' $true '06-gui-verify.log' (Join-Path $root 'tools\gui-verify.ps1') @('-Port', "$Port") | Out-Null

# ---- real-device pre-clean (once, before the M0..M3 section) -----------------
# Kill leftovers of THIS repo only (never uacsimd / anything else), then drop
# every usbip kernel import. Both failures are ignored: a clean machine has
# neither leftover processes nor imports.
Write-Output '== real-device pre-clean: taskkill phantombt.exe /F + usbip detach -a (failures ignored; uacsimd never touched)'
cmd /c "taskkill /IM phantombt.exe /F >nul 2>&1"
if ($usbipExe) { & $usbipExe detach -a *> $null }
else { Write-Output '== real-device pre-clean: usbip.exe not found -- detach -a skipped (real-device legs will SKIP)' }

# =========================================================== 07 m0-realdevice
# run-m0.ps1 exit codes: 0 = loopback PASS AND realdevice=PASS; 1 = FAIL
# (build/loopback/realdevice); 3 = realdevice SKIP. The stage therefore
# requires realdevice=PASS exactly -- rc 0 is unreachable otherwise.
$m0json = Join-Path $resDir 'm0.json'
if (Test-Path $m0json) { Remove-Item $m0json -Force -ErrorAction SilentlyContinue }
$log = Join-Path $logDir '07-m0-realdevice.log'
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& powershell -NoProfile -File (Join-Path $root 'tools\run-m0.ps1') -Port $Port *> $log
$rc = $LASTEXITCODE
$sw.Stop()
$rd = ''
try { $rd = (Get-Content $m0json -Raw | ConvertFrom-Json).realdevice } catch {}
$status = 'FAIL'; $detail = ('exit ' + $rc + ' realdevice=' + $(if ($rd) { $rd } else { 'unknown' }))
if ($rc -eq 0) { $status = 'PASS'; $detail = 'realdevice=' + $rd }
elseif ($rc -eq 3) { $status = 'SKIPPED'; $detail = 'realdevice=' + $(if ($rd) { $rd } else { 'unknown' }) }
Write-Stage 'm0-realdevice' $true $status $sw.Elapsed.TotalSeconds $detail $log

# =========================================================== 08 m1-realdevice
# M1 passes on the registry caliber too: run-m1.ps1 rc 0 covers realdevice
# PASS and PASS(registry) (its radio probe falls back to radio-registry).
$m1json = Join-Path $resDir 'm1.json'
if (Test-Path $m1json) { Remove-Item $m1json -Force -ErrorAction SilentlyContinue }
$log = Join-Path $logDir '08-m1-realdevice.log'
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& powershell -NoProfile -File (Join-Path $root 'tools\run-m1.ps1') -Port $Port *> $log
$rc = $LASTEXITCODE
$sw.Stop()
$rd = ''
try { $rd = (Get-Content $m1json -Raw | ConvertFrom-Json).realdevice } catch {}
$status = 'FAIL'; $detail = ('exit ' + $rc + ' realdevice=' + $(if ($rd) { $rd } else { 'unknown' }))
if ($rc -eq 0) { $status = 'PASS'; $detail = 'realdevice=' + $rd }
elseif ($rc -eq 3) { $status = 'SKIPPED'; $detail = 'realdevice=' + $(if ($rd) { $rd } else { 'unknown' }) }
Write-Stage 'm1-realdevice' $true $status $sw.Elapsed.TotalSeconds $detail $log

# =========================================================== 09 m2/m3-realdevice
# Attempted and recorded, never blocking (M2/M3 real-device legs are the
# youngest milestones; a FAIL here must not fail the whole run).
foreach ($m in @('m2', 'm3')) {
  $mjson = Join-Path $resDir ($m + '.json')
  if (Test-Path $mjson) { Remove-Item $mjson -Force -ErrorAction SilentlyContinue }
  $log = Join-Path $logDir ('09-' + $m + '-realdevice.log')
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  & powershell -NoProfile -File (Join-Path $root ('tools\run-' + $m + '.ps1')) -Port $Port *> $log
  $rc = $LASTEXITCODE
  $sw.Stop()
  $rd = ''
  try { $rd = (Get-Content $mjson -Raw | ConvertFrom-Json).realdevice } catch {}
  $status = 'FAIL'; $detail = ('exit ' + $rc + ' realdevice=' + $(if ($rd) { $rd } else { 'unknown' }))
  if ($rc -eq 0) { $status = 'PASS'; $detail = 'realdevice=' + $rd }
  elseif ($rc -eq 3) { $status = 'SKIPPED'; $detail = 'realdevice=' + $(if ($rd) { $rd } else { 'unknown' }) }
  Write-Stage ($m + '-realdevice') $false $status $sw.Elapsed.TotalSeconds $detail $log
}

# ================================================================= summary
$wall = ((Get-Date) - $t0).TotalSeconds
$sumSecs = 0.0
foreach ($s in $script:stages) { $sumSecs += $s.Sec }
$blockingFails = @($script:stages | Where-Object { $_.Blocking -and $_.Status -eq 'FAIL' })

$md = New-Object System.Collections.Generic.List[string]
$md.Add('# Phantom-BT full-flow test report')
$md.Add('')
$md.Add('- date: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
$md.Add('- port: ' + $Port + ' (serve/usbip; usbip default 3240 is reserved by uacsimd on this machine -- never killed)')
$md.Add('- elevated shell: ' + $admin + '  usbip.exe: ' + $(if ($usbipExe) { $usbipExe } else { 'not found' }))
$md.Add('- wall time: ' + [math]::Round($wall, 1) + 's (sum of stages: ' + [math]::Round($sumSecs, 1) + 's)')
$md.Add('- exit rule: any blocking stage FAIL -> tools/run-all.ps1 exits 1 (SKIPPED never fails; m2/m3 are recorded, never blocking)')
$md.Add('- stage logs: integration-results/full-test-logs/')
$md.Add('')
$md.Add('| # | stage | status | secs | blocking | detail |')
$md.Add('|---|-------|--------|------|----------|--------|')
$i = 0
foreach ($s in $script:stages) {
  $i++
  $md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} |' -f $i, $s.Name, $s.Status, $s.Sec, $s.Blocking, ($s.Detail -replace '\|', '/')))
}
$md.Add('')
$md.Add('## Failed-stage tails (last 30 log lines each)')
$md.Add('')
$tails = @($script:stages | Where-Object { $_.Status -eq 'FAIL' })
if ($tails.Count -eq 0) {
  $md.Add('(none)')
  $md.Add('')
} else {
  foreach ($s in $tails) {
    $md.Add('### ' + $s.Name)
    $md.Add('')
    $md.Add('```')
    $tail = Get-LogTail $s.Log
    if ($tail) { $md.Add($tail) } else { $md.Add('(log empty or missing: ' + $s.Log + ')') }
    $md.Add('```')
    $md.Add('')
  }
}
$report = Join-Path $resDir 'full-test-report.md'
[System.IO.File]::WriteAllLines($report, $md, (New-Object System.Text.UTF8Encoding($false)))

Write-Output ''
Write-Output ('===================== full-flow summary (wall ' + [math]::Round($wall, 1) + 's) =====================')
'{0,-18} {1,-8} {2,8} {3,-9} {4}' -f 'stage', 'status', 'secs', 'blocking', 'detail'
foreach ($s in $script:stages) {
  '{0,-18} {1,-8} {2,8} {3,-9} {4}' -f $s.Name, $s.Status, $s.Sec, $s.Blocking, $s.Detail
}
Write-Output ('report: ' + $report)
if ($blockingFails.Count -gt 0) {
  Write-Output ('FAILED: ' + $blockingFails.Count + ' blocking stage(s): ' + (($blockingFails | ForEach-Object { $_.Name }) -join ', '))
  exit 1
}
Write-Output 'ALL BLOCKING STAGES GREEN (SKIPPED does not fail)'
exit 0
