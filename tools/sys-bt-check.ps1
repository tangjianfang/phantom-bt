# Drive a live serve (serve --load-bt) simulator via the control pipe:
# sys-bt list round-trip, using the CLI's own pipe client (`phantombt cmd` --
# the same CommandClient path tools/run-m1.ps1 and check-tools use live).
# ASCII-only source; UTF-8 output. Exits 1 if any round-trip fails.
# NOTE: never bind the usbip server without --port: the CLI default 3240 is
# reserved by uacsimd on this machine (port rule in tools/run-all.ps1).
$root = Split-Path -Parent $PSScriptRoot
$out = "$root/integration-results/sysbt-cli.txt"
$exe = "$root/build/release/src/phantombt.exe"
$port = 3425
$lines = New-Object System.Collections.Generic.List[string]
$p = Start-Process -FilePath $exe -ArgumentList 'serve','--port',"$port",'--load-bt','--exit-after-ms','20000' `
  -WorkingDirectory $root -RedirectStandardError "$env:TEMP\pb.err" -PassThru -WindowStyle Hidden
$fail = 0
try {
  # Wait for the control pipe (`phantombt cmd` itself only retries ~1s)
  $pipeReady = $false
  foreach ($i in 1..30) {
    if ($p.HasExited) { break }
    $null = & $exe cmd "status" 2>&1
    if ($LASTEXITCODE -eq 0) { $pipeReady = $true; break }
    Start-Sleep -Milliseconds 500
  }
  if (-not $pipeReady) { throw "control pipe not ready (serve log: $env:TEMP\pb.err)" }
  foreach ($cmd in @('sys-bt list','ble-clear','unload-all','status')) {
    $lines.Add('> ' + $cmd)
    $reply = @(& $exe cmd $cmd 2>&1)
    $lines.AddRange([string[]]($reply | ForEach-Object { '  ' + $_ }))
    # `cmd` exits 0 even on ERR replies; the reply text is what gets checked
    # (same contract as run-m1.ps1): every reply must be OK-framed, not ERR.
    if (@($reply | Where-Object { $_ -like 'ERR*' }).Count -gt 0) { $fail++ }
  }
} catch {
  $lines.Add('EXC ' + $_.Exception.Message)
  $fail++
} finally {
  Wait-Process -Id $p.Id -Timeout 25 -ErrorAction SilentlyContinue
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
[System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
if ($fail -ne 0) { Write-Output "FAILED: $fail command(s) errored (see $out)"; exit 1 }
Write-Output 'done'
exit 0
