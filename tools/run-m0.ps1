param([switch]$SkipRealDevice, [int]$Port = 3425)
# M0 acceptance: USB HID full chain
# 1) build  2) loopback selftest (no kernel deps, always runs)
# 3) real device (default): serve --load gamepad --attach --probe winrt-probe hid
#    requires usbip-win2 driver + admin; otherwise SKIP.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path "$root/integration-results" | Out-Null
$results = @{ milestone = "M0"; build = "FAIL"; loopback = "FAIL"; realdevice = "SKIP" }

& powershell -NoProfile -File "$root/tools/build.ps1" -Config Release *> $null
if ($LASTEXITCODE -ne 0) {
  $results | ConvertTo-Json | Set-Content "$root/integration-results/m0.json"
  Write-Error "build failed"; exit 1
}
$results.build = "PASS"

& "$root/build/release/src/phantombt.exe" selftest
$results.loopback = if ($LASTEXITCODE -eq 0) { "PASS" } else { "FAIL" }

if (-not $SkipRealDevice) {
  $usbip = Get-Command usbip.exe -ErrorAction SilentlyContinue
  if (-not $usbip) {
    $usbip = Get-Item "C:\Program Files\USBip\usbip.exe" -ErrorAction SilentlyContinue
  }
  $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  if ($usbip -and $admin) {
    $probeExe = "$root/build/release/src/winrt-probe.exe"
    if (Test-Path $probeExe) {
      $probeCmd = ('"{0}" hid --vid 0x1209 --pid 0xB007 --usage-page 0xFF00 --usage 0x01 --wait-ms 20000' -f $probeExe)
      & "$root/build/release/src/phantombt.exe" serve --port $Port --load "$root/profiles/gamepad.json" --attach --probe $probeCmd --exit-after-ms 2000
      $results.realdevice = if ($LASTEXITCODE -eq 0) { "PASS" } else { "FAIL" }
    } else {
      $results.realdevice = "SKIP(no probe)"  # no cppwinrt environment
    }
  } else {
    $results.realdevice = "SKIP(no admin or usbip.exe)"
  }
}

$results | ConvertTo-Json | Set-Content "$root/integration-results/m0.json"
$results | ConvertTo-Json | Write-Output
if ($results.loopback -ne "PASS") { exit 1 }
if ($results.realdevice -like "FAIL*") { exit 1 }
if ($results.realdevice -like "SKIP*") { exit 3 }
exit 0
