param([switch]$SkipRealDevice)
# M3 acceptance: BLE HID keyboard end-to-end over real Windows BT stack
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path "$root/integration-results" | Out-Null
$results = @{ milestone = "M3"; build = "FAIL"; loopback = "FAIL"; realdevice = "SKIP" }

& powershell -NoProfile -File "$root/tools/build.ps1" -Config Release *> $null
if ($LASTEXITCODE -ne 0) {
  $results | ConvertTo-Json | Set-Content "$root/integration-results/m3.json"
  Write-Error "build failed"; exit 1
}
$results.build = "PASS"

# loopback: FakeHost full chain (unit tests)
& "$root/build/release/tests/phantombt-tests.exe" e2e_ *> $null
$results.loopback = if ($LASTEXITCODE -eq 0) { "PASS" } else { "FAIL" }

if (-not $SkipRealDevice) {
  $usbip = Get-Command usbip.exe -ErrorAction SilentlyContinue
  if (-not $usbip) { $usbip = Get-Item "C:\Program Files\USBip\usbip.exe" -ErrorAction SilentlyContinue }
  $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  if ($usbip -and $admin) {
    $probeExe = "$root/build/release/src/winrt-probe.exe"
    if (Test-Path $probeExe) {
      # 1) watcher sees advertisement
      $serveProc = Start-Process -FilePath "$root/build/release/src/phantombt.exe" `
        -ArgumentList "serve","--load-bt","--ble","$root/profiles/ble_keyboard.json","--exit-after-ms","60000" `
        -PassThru -WindowStyle Hidden
      Start-Sleep -Milliseconds 800
      $usbipExe = if ($usbip -is [System.IO.FileInfo]) { $usbip.FullName } else { $usbip.Source }
      & $usbipExe attach -r 127.0.0.1 -p 3240 -b 7-1 2>&1 | Out-Null
      & $probeExe watcher --expect-name PhantomKb --duration-ms 10000 | Tee-Object -Variable w
      $watchRc = $LASTEXITCODE
      # 2) connect + GATT + HID node
      & $probeExe ble --address 5544332211C0 --wait-ms 20000 | Tee-Object -Variable b
      $bleRc = $LASTEXITCODE
      Stop-Process -Id $serveProc.Id -Force -ErrorAction SilentlyContinue
      $results.realdevice = if ($watchRc -eq 0 -and $bleRc -eq 0) { "PASS" } else { "FAIL(w=$watchRc,b=$bleRc)" }
    } else { $results.realdevice = "SKIP(no probe)" }
  } else { $results.realdevice = "SKIP(no admin or usbip.exe)" }
}

$results | ConvertTo-Json | Set-Content "$root/integration-results/m3.json"
$results | ConvertTo-Json | Write-Output
if ($results.loopback -ne "PASS") { exit 1 }
if ($results.realdevice -like "FAIL*") { exit 1 }
if ($results.realdevice -like "SKIP*") { exit 3 }
exit 0
