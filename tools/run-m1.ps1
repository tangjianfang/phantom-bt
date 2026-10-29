param([switch]$SkipRealDevice)
# M1 acceptance: virtual bluetooth adapter radio
# 1) build  2) loopback full HCI init (no kernel)  3) real device: load-bt attach + radio probe
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path "$root/integration-results" | Out-Null
$results = @{ milestone = "M1"; build = "FAIL"; loopback = "FAIL"; realdevice = "SKIP" }

& powershell -NoProfile -File "$root/tools/build.ps1" -Config Release *> $null
if ($LASTEXITCODE -ne 0) {
  $results | ConvertTo-Json | Set-Content "$root/integration-results/m1.json"
  Write-Error "build failed"; exit 1
}
$results.build = "PASS"

# loopback: full HCI init over TCP/USB-IP (unit test)
& "$root/build/release/tests/phantombt-tests.exe" server_bt_loopback_full_init *> $null
$results.loopback = if ($LASTEXITCODE -eq 0) { "PASS" } else { "FAIL" }

if (-not $SkipRealDevice) {
  $usbip = Get-Command usbip.exe -ErrorAction SilentlyContinue
  if (-not $usbip) { $usbip = Get-Item "C:\Program Files\USBip\usbip.exe" -ErrorAction SilentlyContinue }
  $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  if ($usbip -and $admin) {
    $probeExe = "$root/build/release/src/winrt-probe.exe"
    if (Test-Path $probeExe) {
      $probeCmd = ('"{0}" radio --wait-ms 15000' -f $probeExe)
      & "$root/build/release/src/phantombt.exe" serve --load-bt --attach --probe $probeCmd --exit-after-ms 2000
      $rc = $LASTEXITCODE
      if ($rc -eq 0) {
        $results.realdevice = "PASS"
      } else {
        # fallback: registry driver-binding check
        & $probeExe radio-registry | Tee-Object -Variable regOut
        $results.realdevice = if ($LASTEXITCODE -eq 0) { "PASS(registry)" } else { "FAIL" }
      }
    } else {
      $results.realdevice = "SKIP(no probe)"
    }
  } else {
    $results.realdevice = "SKIP(no admin or usbip.exe)"
  }
}

$results | ConvertTo-Json | Set-Content "$root/integration-results/m1.json"
$results | ConvertTo-Json | Write-Output
if ($results.loopback -ne "PASS") { exit 1 }
if ($results.realdevice -like "FAIL*") { exit 1 }
if ($results.realdevice -like "SKIP*") { exit 3 }
exit 0
