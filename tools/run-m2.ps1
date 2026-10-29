param([switch]$SkipRealDevice)
# M2 acceptance: BLE advertising discovery
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path "$root/integration-results" | Out-Null
$results = @{ milestone = "M2"; build = "FAIL"; loopback = "FAIL"; realdevice = "SKIP" }

& powershell -NoProfile -File "$root/tools/build.ps1" -Config Release *> $null
if ($LASTEXITCODE -ne 0) {
  $results | ConvertTo-Json | Set-Content "$root/integration-results/m2.json"
  Write-Error "build failed"; exit 1
}
$results.build = "PASS"

# loopback: scan + advertising report via controller tick (unit tests)
& "$root/build/release/tests/phantombt-tests.exe" controller_scan; $rc1 = $LASTEXITCODE
& "$root/build/release/tests/phantombt-tests.exe" controller_le_create; $rc2 = $LASTEXITCODE
$results.loopback = if ($rc1 -eq 0 -and $rc2 -eq 0) { "PASS" } else { "FAIL" }

if (-not $SkipRealDevice) {
  $usbip = Get-Command usbip.exe -ErrorAction SilentlyContinue
  if (-not $usbip) { $usbip = Get-Item "C:\Program Files\USBip\usbip.exe" -ErrorAction SilentlyContinue }
  $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  if ($usbip -and $admin) {
    $probeExe = "$root/build/release/src/winrt-probe.exe"
    if (Test-Path $probeExe) {
      # serve 加载蓝牙适配器与 BLE 键盘广播；探针在 attach 前启动等待
      $serveProc = Start-Process -FilePath "$root/build/release/src/phantombt.exe" `
        -ArgumentList "serve","--load-bt","--ble","$root/profiles/ble_keyboard.json","--exit-after-ms","25000" `
        -PassThru -WindowStyle Hidden
      Start-Sleep -Milliseconds 800
      # attach via usbip
      $usbipExe = if ($usbip -is [System.IO.FileInfo]) { $usbip.FullName } else { $usbip.Source }
      & $usbipExe attach -r 127.0.0.1 -p 3240 -b 7-1 2>&1 | Out-Null
      & $probeExe watcher --expect-name PhantomKb --duration-ms 10000 | Tee-Object -Variable watchOut
      $watchRc = $LASTEXITCODE
      # 广播注入：命名管道暂缺 → 直接由 serve 侧预载（此处用 --probe 模式替代）
      Stop-Process -Id $serveProc.Id -Force -ErrorAction SilentlyContinue
      $results.realdevice = if ($watchRc -eq 0) { "PASS" } else { "FAIL($watchOut)" }
    } else { $results.realdevice = "SKIP(no probe)" }
  } else { $results.realdevice = "SKIP(no admin or usbip.exe)" }
}

$results | ConvertTo-Json | Set-Content "$root/integration-results/m2.json"
$results | ConvertTo-Json | Write-Output
if ($results.loopback -ne "PASS") { exit 1 }
if ($results.realdevice -like "FAIL*") { exit 1 }
if ($results.realdevice -like "SKIP*") { exit 3 }
exit 0
