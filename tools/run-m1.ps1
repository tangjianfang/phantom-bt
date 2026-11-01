param([switch]$SkipRealDevice, [int]$Port = 3425)
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
    $pbExe = "$root/build/release/src/phantombt.exe"
    if ((Test-Path $probeExe) -and (Test-Path $pbExe)) {
      # Multi-radio policy: Windows keeps only ONE active BT adapter (BTHUSB
      # event 6). Disable the built-in physical radio BEFORE attach
      # (`phantombt cmd sys-bt disable` -> src/core/bt_radio.cpp) and restore it
      # on exit. The control pipe is created inside serve BEFORE it attaches
      # (src/cli/main.cpp run_serve), so serve runs here in the background
      # WITHOUT --attach; the script sends attach over the pipe after disabling.
      $serveLog = "$root/integration-results/m1-serve.log"
      $serve = Start-Process -FilePath $pbExe `
        -ArgumentList 'serve', '--port', "$Port", '--load-bt', '--exit-after-ms', '120000' `
        -WorkingDirectory $root -RedirectStandardError $serveLog -PassThru -WindowStyle Hidden
      $physDisabled = $false
      try {
        # Wait for the control pipe (`phantombt cmd` itself only retries ~1s)
        $pipeReady = $false
        foreach ($i in 1..30) {
          if ($serve.HasExited) { break }
          $null = & $pbExe cmd "status" 2>&1
          if ($LASTEXITCODE -eq 0) { $pipeReady = $true; break }
          Start-Sleep -Milliseconds 500
        }
        if (-not $pipeReady) { throw "control pipe not ready (serve log: $serveLog)" }

        # Disable only if some physical radio is currently enabled, and only
        # restore then -- respect a pre-existing disabled state (row format:
        # simulator.cpp cmd_sys_bt). `cmd` exits 0 even on ERR replies, so the
        # reply text is what gets checked.
        $btList = & $pbExe cmd "sys-bt list" 2>&1
        $hadEnabledPhys = (@($btList | Where-Object { $_ -match '\[enabled\].*RADIO' })).Count -gt 0
        if ($hadEnabledPhys) {
          $disOut = (& $pbExe cmd "sys-bt disable" 2>&1) -join ' '
          if ($disOut -match 'disabled [1-9]\d* radio') { $physDisabled = $true }
          else { Write-Warning "sys-bt disable not confirmed: $disOut" }
        }

        # Attach the virtual adapter (busid from the device table rows
        # "DEV <busid> 0b00:0001 ...")
        $devRows = & $pbExe cmd "list" 2>&1
        $busid = $null
        foreach ($row in $devRows) {
          if ($row -match '^DEV (\S+) 0b00:0001 ') { $busid = $Matches[1]; break }
        }
        if (-not $busid) { throw "no 0b00:0001 device in cmd list: $($devRows -join ' | ')" }
        $attOut = (& $pbExe cmd "attach $busid" 2>&1) -join ' '
        if ($attOut -notmatch 'attached') { Write-Warning "attach: $attOut" }

        # Radio probe: prefers the virtual "Phantom-BT Radio" (src/probes/main.cpp,
        # R-110: taking the first BT radio would false-pass on the Intel radio)
        & $probeExe radio --wait-ms 15000
        $rc = $LASTEXITCODE
        if ($rc -eq 0) {
          $results.realdevice = "PASS"
        } else {
          # fallback: registry driver-binding check
          & $probeExe radio-registry | Tee-Object -Variable regOut
          $results.realdevice = if ($LASTEXITCODE -eq 0) { "PASS(registry)" } else { "FAIL" }
        }
      } catch {
        Write-Warning "realdevice: $($_.Exception.Message)"
        $results.realdevice = "FAIL"
      } finally {
        # Restore the physical radio while serve still owns the pipe; if the
        # pipe is gone, spin up a short-lived serve to host it and retry --
        # never leave the user's physical BT radio disabled.
        if ($physDisabled) {
          $enOut = (& $pbExe cmd "sys-bt enable" 2>&1) -join ' '
          if ($enOut -notmatch 'enabled [1-9]\d* radio') {
            Write-Warning "sys-bt enable via pipe failed: $enOut"
            $spare = Start-Process -FilePath $pbExe `
              -ArgumentList 'serve', '--port', ($Port + 1), '--exit-after-ms', '8000' `
              -WorkingDirectory $root -WindowStyle Hidden -PassThru
            foreach ($i in 1..10) {
              if ($spare.HasExited) { break }
              Start-Sleep -Milliseconds 500
              $enOut = (& $pbExe cmd "sys-bt enable" 2>&1) -join ' '
              if ($enOut -match 'enabled [1-9]\d* radio') { break }
            }
            if ($enOut -notmatch 'enabled [1-9]\d* radio') {
              Write-Warning 'phys radio restore FAILED; re-enable manually: phantombt cmd "sys-bt enable"'
            }
            if (-not $spare.HasExited) { Stop-Process -Id $spare.Id -Force -ErrorAction SilentlyContinue }
          }
        }
        # Detach before killing serve: a hard kill leaves the kernel-side vhci
        # import marked "in use" (stale) and the next run's attach then reports
        # "already imported by another connection". Clean self-exit removes the
        # import; force-kill does not -- so detach explicitly first.
        if (-not $serve.HasExited) {
          if ($busid) { $null = & $pbExe cmd "detach $busid" 2>&1 }
          Stop-Process -Id $serve.Id -Force -ErrorAction SilentlyContinue
        }
        Wait-Process -Id $serve.Id -Timeout 10 -ErrorAction SilentlyContinue
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
