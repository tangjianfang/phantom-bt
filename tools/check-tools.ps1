# tools/check-tools.ps1 -- entry-level automated checks for phantombt.exe /
# phantombt-gui.exe (the inventory rows that "no unit test spawns the exe").
# Covers: CLI entry dispatch (usage/unknown/selftest/cmd-no-arg), serve flag
# combos (--port/--load/--load-bt/--ble/--exit-after-ms, bad flag, bad profile),
# the `cmd` pipe client against a live serve (and rc=1 with no bench), doctor
# output/exit-code semantics, GUI pages 0-2 button paths (real WM_LBUTTONUP
# clicks, same delivery as gui-verify.ps1), and driving the GUI's embedded
# PipeServer/usbip server externally via `phantombt cmd` + TCP.
#
# Not automatable here (see notes): serve --probe end-to-end (driving an external
# probe process against the mounted device; its safe facets ARE covered: probe-skip
# exit codes in tests/unit/test_cli_cov.cpp:69-82, the injector payload shape in
# test_entry_cov.cpp entry_probe_inject_payload_shape, and the cmd_attach core in
# section H below) -- serve --attach end-to-end IS automated since section K1
# (it mounts the serve's OWN loopback server, same real import as section H), and
# `cmd` exit code on ERR replies (known gap, src/cli/main.cpp:246 returns 0).
#
# Section G (round 1B) adds the tooling self-checks: syntax of every tools/*.ps1,
# wiring contracts of run-tests/coverage/coverage-gate, serve-vs-attach port
# consistency of run-m0..m3, run-m3 BLE address vs profiles/ble_keyboard.json,
# gui-verify portability+structure, sys-bt-check failure-path presence, cov_gaps
# gate contract + foreign/excluded-dir classification on synthetic cobertura XML,
# ci.yml YAML/step structure + whether .github is committed, and the one-off
# tools (make_icon compile/render, create-repo.bat structure-only). Items listed
# in $knownDefectIds inside section G FAIL on the current tree by design (audited
# unfixed gaps); they flip to PASS when the fix lands.
#
# Section H (sim-control real-path batch) automates the three real-machine rows
# the notes above left open -- pipe-driven `attach` (same Simulator::cmd_attach
# the serve --attach flag calls), kernel unbind via `detach`, and the guarded
# physical-radio toggle with baseline restore. serve --attach/--probe end-to-end
# remains a real-hardware leg. H5 (round 3B, bt_radio row) proves the VID_0B00
# exclusion LIVE: while the virtual BT adapter is kernel-attached (it lands in
# the Bluetooth setup class via its 0xE0/01/01 class codes), `sys-bt list` must
# gain no VID_0B00 row and keep its RADIO rows unchanged -- without the
# bt_radio.cpp:48 exclusion our own adapter would be listed as a physical RADIO
# (its USB\ id passes is_physical_radio_id) and sys-bt disable would toggle it.
#
# Section I (round 2) completes what round 1 left open on the same rows: a real
# serve started with TWO --load flags and driven through `phantombt cmd` for a
# full session (exe-level accumulation + inject + status + unload-all), the GUI
# "load HID" (101) and "load BLE advertise" (201) file-dialog buttons this file
# previously listed as unautomatable (the click is POSTed, not sent: PbButton
# routes WM_LBUTTONUP synchronously -- src/gui/theme.cpp:59 -- so a SendMessage
# click would block the caller inside the modal GetOpenFileNameW loop forever;
# the owned #32770 dialog is then located, its filename Edit filled and IDOK
# posted), inject (106) with a real listview selection + hex from edit 105,
# ble-stop (202) with a listbox selection, and a wire-level OP_REQ_DEVLIST
# round trip against the GUI's embedded usbip server (round 1 only proved the
# TCP port accepts connections).
#
# Section K (round 3B) completes the control-command rows at the entry level:
# `serve --attach` END TO END (the startup flag path, src/cli/main.cpp:96-111 --
# attaches the serve's OWN loopback usbip server into the kernel, previously
# listed as a manual-only leg), its no-usbip skip / unelevated failure legs
# (serve must log the failure and keep serving, rc 0), and the GUI
# global-settings sys-bt buttons (nav 14 -> page 5, buttons 401/402) with a real
# radio state round trip + baseline restore. -SkipReal skips the kernel- and
# radio-touching items of sections H and K.
#
# Section M (round 3) adds process-level checks for winrt-probe.exe (the four
# probe rows that had zero automation: hid / radio winrt+registry / watcher /
# ble) -- usage contract, hid discovery-timeout, the two radio calibers (cross-
# checked against sys-bt for the M1 dual-caliber concern), watcher graceful
# fail + duration, ble arg validation and connect failure. The positive probe
# legs stay real-device acceptance (run-m0..m3).
#
# Output: one PASS/FAIL/SKIP line per check. Exit codes: 0 = every executed
# check passed (SKIP does not fail), 1 = at least one FAIL, 3 = environment not
# ready (executables missing; everything SKIPped). ASCII-only source, UTF-8
# output file, same conventions as gui-verify.ps1.
param([switch]$SkipGui, [switch]$SkipReal)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $root 'build/release/src/phantombt.exe'
$gui = Join-Path $root 'build/release/src/phantombt-gui.exe'
$out = Join-Path $root 'integration-results/check-tools.txt'
$lines = New-Object System.Collections.Generic.List[string]
$script:fail = 0
$script:skip = 0

function Check([string]$name, [bool]$ok, [string]$detail) {
  if ($ok) { $script:lines.Add('PASS ' + $name) }
  else { $script:lines.Add('FAIL ' + $name + $(if ($detail) { '  [' + $detail + ']' } else { '' })); $script:fail = $script:fail + 1 }
}
function Skip([string]$name, [string]$why) {
  $script:lines.Add('SKIP ' + $name + '  [' + $why + ']'); $script:skip = $script:skip + 1
}

function Get-FreePort {
  $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
  $l.Start()
  $p = ($l.LocalEndpoint).Port
  $l.Stop()
  return $p
}
function Test-PipeExists([string]$name) {
  try {
    foreach ($f in [System.IO.Directory]::GetFiles("\\.\pipe\")) {
      if ($f -eq ("\\.\pipe\" + $name)) { return $true }
    }
  } catch {}
  return $false
}

# Run the CLI, capture stdout/stderr (decoded UTF-8; the exe writes UTF-8 bytes),
# enforce a timeout. Returns @{ Rc; Out; Err }.
function Run-Cli([string[]]$ArgList, [int]$TimeoutSec = 30) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $cli
  $psi.Arguments = ($ArgList | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $psi.WorkingDirectory = $root   # relative paths like profiles/vendor.json resolve at repo root
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
  $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
  $psi.CreateNoWindow = $true
  $p = [System.Diagnostics.Process]::Start($psi)
  $outT = $p.StandardOutput.ReadToEndAsync()
  $errT = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    try { $p.Kill() } catch {}
    $p.WaitForExit()
  }
  $p.WaitForExit()
  return @{ Rc = $p.ExitCode; Out = $outT.Result; Err = $errT.Result }
}

# ---------------------------------------------------------------- environment
if (-not (Test-Path $cli)) {
  Skip 'all' ('phantombt.exe not found at ' + $cli + ' -- run tools/build.ps1 first')
  [System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
  $lines | ForEach-Object { Write-Output $_ }
  Write-Output ('SKIP: environment not ready'); exit 3
}

# ===================================================== A. CLI entry dispatch
$r = Run-Cli @()
Check 'cli-entry-no-args-prints-usage-exit2' ($r.Rc -eq 2 -and $r.Err -match 'usage: phantombt') ("rc=" + $r.Rc)

$r = Run-Cli @('no-such-command')
Check 'cli-entry-unknown-command-exit2' ($r.Rc -eq 2 -and $r.Err -match 'unknown command') ("rc=" + $r.Rc)

$r = Run-Cli @('cmd')
Check 'cli-entry-cmd-without-args-exit2' ($r.Rc -eq 2 -and $r.Err -match 'unknown command') ("rc=" + $r.Rc)

$r = Run-Cli @('selftest')
Check 'cli-entry-selftest-loopback-pass' ($r.Rc -eq 0 -and $r.Out -match 'PASS loopback') ("rc=" + $r.Rc + ' out=' + $r.Out.Trim())

# ===================================================== B. serve flag combos
$port = Get-FreePort
$r = Run-Cli @('serve', '--port', "$port", '--load', 'profiles/vendor.json', '--load-bt',
               '--ble', 'profiles/ble_keyboard.json', '--exit-after-ms', '2500') 60
Check 'cli-serve-all-flags-exit0' ($r.Rc -eq 0) ("rc=" + $r.Rc + ' err=' + $r.Err.Trim())
Check 'cli-serve-port-flag-honored' ($r.Err -match ("usbip server on port " + $port)) ($r.Err.Trim())

$r = Run-Cli @('serve', '--bogus-flag', '--exit-after-ms', '500')
Check 'cli-serve-unknown-flag-exit2' ($r.Rc -eq 2 -and $r.Err -match 'unknown flag') ("rc=" + $r.Rc)

$r = Run-Cli @('serve', '--load', 'profiles/no-such-profile.json', '--exit-after-ms', '500') 30
Check 'cli-serve-bad-profile-exit1' ($r.Rc -eq 1 -and $r.Err -match 'load profiles/no-such-profile.json') ("rc=" + $r.Rc)

# ===================================================== C. `cmd` pipe client
$cport = Get-FreePort
$serveErr = Join-Path $env:TEMP 'pb-check-serve.err'
$serve = Start-Process -FilePath $cli -ArgumentList 'serve', '--port', "$cport", '--exit-after-ms', '40000' `
  -WorkingDirectory $root -RedirectStandardError $serveErr -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$serveAlive = -not $serve.HasExited

$r = Run-Cli @('cmd', 'status')
Check 'cli-cmd-status-against-serve' ($r.Rc -eq 0 -and $r.Out -match '^OK usbip-port=') ("rc=" + $r.Rc + ' out=' + $r.Out.Trim())

$r = Run-Cli @('cmd', 'list')
Check 'cli-cmd-list-against-serve' ($r.Rc -eq 0 -and $r.Out -match 'OK-END') ("rc=" + $r.Rc + ' out=' + $r.Out.Trim())

$r = Run-Cli @('cmd', 'sys-bt list')
Check 'cli-cmd-sysbt-list-against-serve' ($r.Rc -eq 0 -and $r.Out -match 'OK-END') ("rc=" + $r.Rc + ' out=' + ($r.Out -replace "`r`n", ' | '))

# Server replies ERR for an unknown command. NOTE: `cmd` still exits 0 on ERR
# replies today (known gap, src/cli/main.cpp:246-247); only the reply shape is
# asserted here so fixing that gap will not be papered over by this check.
$r = Run-Cli @('cmd', 'bogus-command')
Check 'cli-cmd-err-reply-shape' ($r.Out -match '(?m)^ERR') ('out=' + $r.Out.Trim())

if ($serveAlive) { Stop-Process -Id $serve.Id -Force -ErrorAction SilentlyContinue }
Wait-Process -Id $serve.Id -Timeout 10 -ErrorAction SilentlyContinue

# No bench running -> rc=1 with the "no test bench" stderr. Skip if some other
# phantom-bt bench (a user's GUI/serve) happens to own the pipe on this machine.
$pipeGone = $false
foreach ($i in 1..10) {
  Start-Sleep -Milliseconds 500
  if (-not (Test-PipeExists 'phantom-bt')) { $pipeGone = $true; break }
}
if ($pipeGone) {
  $r = Run-Cli @('cmd', 'status')
  Check 'cli-cmd-no-bench-exit1' ($r.Rc -eq 1 -and $r.Err -match 'no test bench') ("rc=" + $r.Rc)
} else {
  Skip 'cli-cmd-no-bench-exit1' 'another phantom-bt bench owns the pipe on this machine'
}

# ===================================================== D. doctor
$r = Run-Cli @('doctor') 60
Check 'cli-doctor-exit-code-domain' ($r.Rc -eq 0 -or $r.Rc -eq 1) ("rc=" + $r.Rc)
$marked = ($r.Out -split "`n" | Where-Object { $_ -match '^\[( OK |WARN|INFO|FAIL)\] ' }).Count
Check 'cli-doctor-prints-marked-check-rows' ($marked -ge 5) ("marked rows=" + $marked)
Check 'cli-doctor-prints-summary-line' ($r.Out -match '(?m)^== \d+ .+ ==') ''
$hasWF = (($r.Out -split "`n" | Where-Object { $_ -match '^\[(WARN|FAIL)\] ' }).Count -gt 0)
Check 'cli-doctor-exit-code-matches-warnings' (($r.Rc -eq 1) -eq $hasWF) ("rc=" + $r.Rc + ' warnOrFailRows=' + $hasWF)

# ===================================================== E. GUI pages 0-2 + embedded pipe
if ($SkipGui) {
  Skip 'gui-section' '-SkipGui given (no interactive desktop in this environment)'
} elseif (-not (Test-Path $gui)) {
  Skip 'gui-section' ('phantombt-gui.exe not found at ' + $gui)
} else {
  $sig = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections;
public class G {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageSB(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr FindByTitlePart(string part) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      if (IsWindowVisible(h) && found == IntPtr.Zero) {
        int n = GetWindowTextLength(h);
        if (n > 0) { var sb = new StringBuilder(n + 2); GetWindowText(h, sb, sb.Capacity);
          if (sb.ToString().IndexOf(part) >= 0) { found = h; return false; } } }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static ArrayList Children(IntPtr parent) {
    var list = new ArrayList();
    EnumChildWindows(parent, (h, lp) => { list.Add(h); return true; }, IntPtr.Zero);
    return list;
  }
  public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
  public static int IdOf(IntPtr h) { return GetDlgCtrlID(h); }
  public static string GetText(IntPtr h) {
    IntPtr len = SendMessageW(h, 0x000E, IntPtr.Zero, IntPtr.Zero);
    var sb = new StringBuilder(len.ToInt32() + 2);
    SendMessageSB(h, 0x000D, (IntPtr)(len.ToInt32() + 2), sb);
    return sb.ToString();
  }
  public static RECT RectOf(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }
  public static void Click(IntPtr h) {
    RECT r = RectOf(h);
    int cx = (r.L + r.R) / 2, cy = (r.T + r.B) / 2;
    // Real click path: WM_LBUTTONUP in client coords (same delivery gui-verify.ps1
    // uses, exercising the PbButton owner-draw proc -> WM_COMMAND -> on_command).
    SendMessageW(h, 0x202, IntPtr.Zero, (IntPtr)(((cy - r.T) << 16) | ((cx - r.L) & 0xFFFF)));
  }
}
'@
  Add-Type -TypeDefinition $sig

  function Find-ByCtrlId([IntPtr]$parent, [int]$id) {
    foreach ($k in [G]::Children($parent)) { if ([G]::IdOf($k) -eq $id) { return $k } }
    return [IntPtr]::Zero
  }
  function Read-LogPage([IntPtr]$logPage) {
    foreach ($k in [G]::Children($logPage)) { if ([G]::ClassOf($k) -eq 'Edit') { return [G]::GetText($k) } }
    return ''
  }

  cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
  Start-Sleep -Milliseconds 600
  Start-Process -FilePath $gui -WorkingDirectory $root
  Start-Sleep -Seconds 3
  $mw = [G]::FindByTitlePart('phantom-bt')
  Check 'gui-main-window-appears' ($mw -ne [IntPtr]::Zero)

  if ($mw -ne [IntPtr]::Zero) {
    # --- embedded PipeServer driven externally via `phantombt cmd` (matrix row 7)
    $r = Run-Cli @('cmd', 'status')
    $guiPort = 0
    if ($r.Out -match 'usbip-port=(\d+)') { $guiPort = [int]$matches[1] }
    Check 'gui-pipe-status-via-cli' ($r.Rc -eq 0 -and $r.Out -match '^OK usbip-port=\d+') ("rc=" + $r.Rc + ' out=' + $r.Out.Trim())

    $r = Run-Cli @('cmd', 'load-hid profiles/vendor.json')
    Check 'gui-pipe-load-hid-via-cli' ($r.Rc -eq 0 -and $r.Out -match '^OK 7-1') ('out=' + $r.Out.Trim())
    $r = Run-Cli @('cmd', 'list')
    Check 'gui-pipe-list-sees-device' ($r.Out -match 'DEV 7-1 1209:b00d') ('out=' + ($r.Out -replace "`r`n", ' | '))
    $r = Run-Cli @('cmd', 'unload-all')
    Check 'gui-pipe-unload-all-via-cli' ($r.Rc -eq 0 -and $r.Out -match '^OK unloaded') ('out=' + $r.Out.Trim())

    # --- embedded usbip server actually listening on the reported port
    $tcpOk = $false
    if ($guiPort -gt 0) {
      try {
        $tc = New-Object System.Net.Sockets.TcpClient
        $tcpOk = $tc.ConnectAsync('127.0.0.1', $guiPort).Wait(2000)
        $tc.Close()
      } catch { $tcpOk = $false }
    }
    Check 'gui-embedded-usbip-port-listening' $tcpOk ("port=" + $guiPort)

    # --- page 0 (USB devices): nav flips, no-selection buttons log ERR, load-bt works
    $navDev = Find-ByCtrlId $mw 10
    $pageDev = Find-ByCtrlId $mw 1
    $pageBle = Find-ByCtrlId $mw 2
    $pageLog = Find-ByCtrlId $mw 3
    Check 'gui-page-handles-found' ($navDev -ne [IntPtr]::Zero -and $pageDev -ne [IntPtr]::Zero -and $pageBle -ne [IntPtr]::Zero -and $pageLog -ne [IntPtr]::Zero)

    if ($navDev -ne [IntPtr]::Zero -and $pageDev -ne [IntPtr]::Zero) {
      [G]::Click($navDev)
      Start-Sleep -Milliseconds 500
      Check 'gui-device-page-nav-flips-page' ([G]::IsWindowVisible($pageDev))

      # Empty list => attach/detach/inject take the logged ERR branch
      # (on_command, src/gui/main.cpp:356-381) -- never a kernel attach.
      $btnAttach = Find-ByCtrlId $pageDev 103
      $btnDetach = Find-ByCtrlId $pageDev 104
      $btnInject = Find-ByCtrlId $pageDev 106
      $btnLoadBt = Find-ByCtrlId $pageDev 102
      Check 'gui-device-page-buttons-found' ($btnAttach -ne [IntPtr]::Zero -and $btnDetach -ne [IntPtr]::Zero -and $btnInject -ne [IntPtr]::Zero -and $btnLoadBt -ne [IntPtr]::Zero)

      if ($btnAttach -ne [IntPtr]::Zero) {
        [G]::Click($btnAttach); Start-Sleep -Milliseconds 400
        Check 'gui-device-page-attach-no-selection-logged' ((Read-LogPage $pageLog) -match '> attach') ''
      }
      if ($btnDetach -ne [IntPtr]::Zero) {
        [G]::Click($btnDetach); Start-Sleep -Milliseconds 400
        Check 'gui-device-page-detach-no-selection-logged' ((Read-LogPage $pageLog) -match '> detach') ''
      }
      if ($btnInject -ne [IntPtr]::Zero) {
        [G]::Click($btnInject); Start-Sleep -Milliseconds 400
        Check 'gui-device-page-inject-no-selection-logged' ((Read-LogPage $pageLog) -match '> inject') ''
      }
      # Real button -> run_and_show("load-bt") -> device visible over the pipe.
      # busid number depends on prior loads (next_devnum only increments,
      # simulator.cpp:175) so match any 7-N, keyed on the 0b00: adapter prefix.
      if ($btnLoadBt -ne [IntPtr]::Zero) {
        [G]::Click($btnLoadBt); Start-Sleep -Milliseconds 800
        $r = Run-Cli @('cmd', 'list')
        Check 'gui-device-page-load-bt-button-loads' ($r.Out -match 'DEV 7-\d+ 0b00:') ('out=' + ($r.Out -replace "`r`n", ' | '))
      }
    }

    # --- page 1 (BLE): nav flips, type button drives a command, stop is harmless
    $navBle = Find-ByCtrlId $mw 11
    if ($navBle -ne [IntPtr]::Zero -and $pageBle -ne [IntPtr]::Zero) {
      [G]::Click($navBle)
      Start-Sleep -Milliseconds 500
      Check 'gui-ble-page-nav-flips-page' ([G]::IsWindowVisible($pageBle))
      $btnStop = Find-ByCtrlId $pageBle 202
      $btnType = Find-ByCtrlId $pageBle 204
      Check 'gui-ble-page-buttons-found' ($btnStop -ne [IntPtr]::Zero -and $btnType -ne [IntPtr]::Zero)
      if ($btnStop -ne [IntPtr]::Zero) {
        [G]::Click($btnStop); Start-Sleep -Milliseconds 400
        $proc = Get-Process phantombt-gui -ErrorAction SilentlyContinue
        Check 'gui-ble-page-stop-no-selection-harmless' ($null -ne $proc -and $proc.Responding) ''
      }
      if ($btnType -ne [IntPtr]::Zero) {
        [G]::Click($btnType); Start-Sleep -Milliseconds 600
        # type button -> run_and_show("ble-type C0:11:22:33:44:55 hello"); without a
        # BLE link the reply is ERR -- both the command echo and ERR land in the log.
        $logTxt = Read-LogPage $pageLog
        Check 'gui-ble-page-type-button-logs' ($logTxt -match '> ble-type') ''
      }
    }

    # --- page 2 (log): shows the lines the buttons above produced
    $navLog = Find-ByCtrlId $mw 12
    if ($navLog -ne [IntPtr]::Zero -and $pageLog -ne [IntPtr]::Zero) {
      [G]::Click($navLog)
      Start-Sleep -Milliseconds 500
      Check 'gui-log-page-nav-flips-page' ([G]::IsWindowVisible($pageLog))
      $logTxt = Read-LogPage $pageLog
      Check 'gui-log-page-shows-log-lines' ($logTxt.Length -gt 0 -and $logTxt -match 'ERR') ("len=" + $logTxt.Length)
    }

    # --- clean shutdown
    [void][G]::PostMessage($mw, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    $exited = $false
    foreach ($i in 1..10) {
      Start-Sleep -Milliseconds 500
      if ($null -eq (Get-Process phantombt-gui -ErrorAction SilentlyContinue)) { $exited = $true; break }
    }
    Check 'gui-clean-shutdown-on-wmclose' $exited
    cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
  }
}

# ======================================== F. sys-bt deep checks (core/bt_radio row)
# One level deeper than cli-cmd-sysbt-list-against-serve above, on the real exe:
# the full per-line format of `sys-bt list` (simulator.cpp cmd_sys_bt builds
# "[disabled] "/"[enabled]  " + "RADIO "/"child  " + description + " (" +
# instance_id + ")" per radio, or the single "no physical bluetooth radio
# found" line) and the ERR/usage reply for a bad sub-op.
# SAFETY: `sys-bt disable`/`sys-bt enable` flip real physical radio state via
# the SetupAPI class installer (admin required). They are sent ONLY in the two
# guarded safe legs of F2 below (unelevated shell -> OS denies the state change
# with error 5; no radio present -> early return before any installer call).
# The elevated+radio combination (real flip with baseline restore) is section
# H3 sysbt-real-toggle-roundtrip; tools/sys-bt-check.ps1 stays the manual
# admin-path companion.
$sport = Get-FreePort
$serveErr2 = Join-Path $env:TEMP 'pb-check-serve2.err'
$serve2 = Start-Process -FilePath $cli -ArgumentList 'serve', '--port', "$sport", '--exit-after-ms', '30000' `
  -WorkingDirectory $root -RedirectStandardError $serveErr2 -PassThru -WindowStyle Hidden
$sl = $null
foreach ($i in 1..8) {
  Start-Sleep -Milliseconds 400
  $sl = Run-Cli @('cmd', 'sys-bt list')
  if ($sl.Rc -eq 0) { break }
  if ($serve2.HasExited) { break }   # serve died (e.g. port clash): stop waiting
}
$fAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
  ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($sl -and $sl.Rc -eq 0) {
  $rows = @($sl.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -ne '' })
  $framed = ($rows.Count -ge 2 -and $rows[0] -eq 'OK' -and $rows[-1] -eq 'OK-END')
  $body = @(); if ($rows.Count -gt 2) { $body = @($rows[1..($rows.Count - 2)]) }
  $okBody = $false; $detail = ''
  if (-not $framed) {
    $detail = 'framing broken: first=''' + $rows[0] + ''' last=''' + $rows[-1] + ''''
  } elseif ($body.Count -eq 0) {
    $detail = 'empty body between OK and OK-END'
  } elseif ($body.Count -eq 1 -and $body[0] -eq 'no physical bluetooth radio found') {
    $okBody = $true; $detail = 'no physical bluetooth radio on this machine'
  } else {
    $bad = @($body | Where-Object { -not ($_ -cmatch '^\[(enabled|disabled)\] +(RADIO|child) +.*\)$') })
    $okBody = ($bad.Count -eq 0)
    $detail = "$($body.Count) device line(s)"
    if ($bad.Count -gt 0) { $detail = $detail + "; offending: '" + $bad[0] + "'" }
  }
  Check 'sysbt-list-line-format-over-pipe' ($framed -and $okBody) $detail

  $sb = Run-Cli @('cmd', 'sys-bt frobnicate')
  Check 'sysbt-bad-op-err-usage-over-pipe' ($sb.Out -match '(?m)^ERR.*usage') ('out=' + $sb.Out.Trim())

  # ---- F2 (round 2B): the sys-bt state-change error paths over the real pipe.
  # Completes the matrix row's disable/enable axis alongside H3: H3 automates
  # the ELEVATED flip (sysbt-real-toggle-roundtrip); these two legs are
  # deterministic and safe without elevation:
  #   radios present + NOT elevated: device enable/disable is admin-only PnP
  #     policy, so the class installer denies it -- reply must be
  #     "ERR failed: <instance_id>: error N" AND every [disabled]/[enabled]
  #     row must be identical before/after (validated 2026-11-01 with a
  #     restricted-token process: error 5 both ways, state preserved);
  #   no RADIO row at all (any elevation): early return before any installer
  #     call (bt_radio.cpp:77-80) -> "ERR no physical bluetooth radio found".
  # Elevated WITH radios: SKIP -- that combination really flips state and is H3.
  $fRows = @($sl.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } |
    Where-Object { $_ -match '^\[(enabled|disabled)\]\s+RADIO' })
  if ($fRows.Count -eq 0) {
    foreach ($op in @('disable', 'enable')) {
      $fno = Run-Cli @('cmd', ("sys-bt " + $op)) 30
      Check ("sysbt-" + $op + "-no-radio-err-over-pipe") `
        ($fno.Out -match '(?m)^ERR no physical bluetooth radio found') `
        ('out=' + ($fno.Out -replace "`r`n", ' | '))
    }
  } elseif ($fAdmin) {
    Skip 'sysbt-disable-unelevated-err-over-pipe' 'elevated shell: real flip covered by sysbt-real-toggle-roundtrip (H3)'
    Skip 'sysbt-enable-unelevated-err-over-pipe' 'elevated shell: real flip covered by sysbt-real-toggle-roundtrip (H3)'
  } else {
    foreach ($op in @('disable', 'enable')) {
      $fpre = Run-Cli @('cmd', 'sys-bt list') 30
      $ftr = Run-Cli @('cmd', ("sys-bt " + $op)) 60
      $fpost = Run-Cli @('cmd', 'sys-bt list') 30
      $errOk = ($ftr.Out -match '(?m)^ERR failed: .+: error \d+')
      $preRows = @($fpre.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -ne '' })
      $postRows = @($fpost.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -ne '' })
      $stateOk = (($preRows -join '|') -eq ($postRows -join '|'))
      Check ("sysbt-" + $op + "-unelevated-err-over-pipe") ($errOk -and $stateOk) `
        ("errOk=$errOk statePreserved=$stateOk reply='" + $ftr.Out.Trim() + "'")
    }
  }
} else {
  Check 'sysbt-list-line-format-over-pipe' $false 'serve did not answer on the control pipe'
  Check 'sysbt-bad-op-err-usage-over-pipe' $false 'serve did not answer on the control pipe'
  foreach ($op in @('disable', 'enable')) {
    Check ("sysbt-" + $op + "-no-radio-err-over-pipe") $false 'serve did not answer on the control pipe'
    if ($fAdmin) {
      Skip ("sysbt-" + $op + "-unelevated-err-over-pipe") 'elevated shell: real flip is H3; unelevated leg unreachable this run (serve did not answer)'
    } else {
      Check ("sysbt-" + $op + "-unelevated-err-over-pipe") $false 'serve did not answer on the control pipe'
    }
  }
}
if ($serve2 -and -not $serve2.HasExited) { Stop-Process -Id $serve2.Id -Force -ErrorAction SilentlyContinue }
Wait-Process -Id $serve2.Id -Timeout 10 -ErrorAction SilentlyContinue

# tools/sys-bt-check.ps1 is the manual admin-path companion of this row; it must
# at least stay syntactically valid (zero PowerShell parse errors).
$pkT = $null; $pkE = $null
[void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/sys-bt-check.ps1'), [ref]$pkT, [ref]$pkE)
Check 'sysbt-manual-script-parses' ($pkE.Count -eq 0) ((@($pkE) | ForEach-Object { $_.Message }) -join '; ')

# ==================== G. tools/script-level self-checks (round 1B, tooling rows)
# Matrix rows "tools/*.ps1 + cov_gaps.py + ci.yml + one-off tools 自身正确性".
# None of these need a rebuild. Items whose id is listed in $knownDefectIds FAIL
# on the current tree BY DESIGN -- they are the automated exposure of audited,
# unfixed tooling gaps and flip to PASS when the fix lands:
#   coverage-config-plumbing                                (#51)
#   run-m2-attach-consistency / run-m3-attach-consistency    (serve 3425 vs -p 3240)
#   gui-verify-path-portability                             (#52)
#   sys-bt-check-fails-on-error                             (no failure path)
#   cov-gaps-excludes-foreign-and-excluded-dirs             (project_rel, cov_gaps.py:44-48)
#   ci-workflows-committed                                  (.github never committed)
# Deliberately NOT executed here (out of scope for a self-check): the full
# run-tests.ps1 chain and coverage.ps1/coverage-gate.ps1 measurements (each =
# full rebuild + OpenCppCoverage), gui-verify.ps1 full run (this file's section
# E already drives the real GUI), create-repo.bat (creates a public GitHub
# repo), and every real-device leg (hardware + admin + usbip-win2).
# PS 5.1 note: native stderr redirects can turn into terminating errors under
# $ErrorActionPreference='Stop'; this section only inspects exit codes and
# stdout, so it runs under 'Continue'.
$ErrorActionPreference = 'Continue'
$knownDefectIds = @('coverage-config-plumbing', 'run-m2-attach-consistency', 'run-m3-attach-consistency',
                    'gui-verify-path-portability', 'sys-bt-check-fails-on-error',
                    'cov-gaps-excludes-foreign-and-excluded-dirs', 'ci-workflows-committed',
                    'readme-matrix-syncs-with-results')

# ---- G0. every tools/*.ps1 parses (syntax of the whole tool dir, not just one)
$gSyntaxBad = @()
foreach ($f in (Get-ChildItem (Join-Path $root 'tools') -Filter '*.ps1')) {
  $gt = $null; $ge = $null
  [void][System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$gt, [ref]$ge)
  if ($ge -and @($ge).Count -gt 0) { $gSyntaxBad += ($f.Name + ': ' + $ge[0].Message) }
}
Check 'tools-all-ps1-parse-clean' ($gSyntaxBad.Count -eq 0) ($gSyntaxBad -join '; ')

# ---- G1. run-tests.ps1 chain wiring (full dynamic chain = rebuild, out of scope):
# unit exe + m0..m3 loopback via -SkipRealDevice, tolerating ONLY rc 3 (SKIP).
$rt = Get-Content (Join-Path $root 'tools/run-tests.ps1') -Raw
Check 'run-tests-chain-structure' (($rt -match '-SkipRealDevice') -and
  ($rt -match '\$rc -ne 0 -and \$rc -ne 3') -and
  ($rt -match 'phantombt-tests\.exe') -and ($rt -match 'build failed')) `
  'tolerance/chain wiring changed; re-audit tools/run-tests.ps1:11-16'

# ---- G2. coverage.ps1 -Config plumbing [known-defect #51]: the exe OpenCppCoverage
# MEASURES must derive from -Config (what line 14 builds). Today line 24 measures
# a hardcoded build\debug exe while the $exe computed from -Config (line 16) is
# never used for measurement: -Config Release measures a stale debug binary.
$cv = Get-Content (Join-Path $root 'tools/coverage.ps1') -Raw
$measured = ([regex]::Matches($cv, '(?m)^\s*--\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Last 1)
Check 'coverage-config-plumbing' (($measured -ne $null) -and ($measured -match 'Config')) `
  ('builds -Config (line 14) but measures hardcoded "' + $measured + '" (line 24) [known-defect #51, docs/global-review-2026-11-01.md]')

# ---- G3. coverage-gate.ps1 keeps its in-chain assertions (and unlike coverage.ps1
# its measured exe DOES follow -Config, line 48).
$cgp = Get-Content (Join-Path $root 'tools/coverage-gate.ps1') -Raw
Check 'coverage-gate-structure' (($cgp -match '\$covExit -ne 0 -and \$covExit -ne 3') -and
  ($cgp -match 'coverage report not produced') -and ($cgp -match 'python not found') -and
  ($cgp -match '\$ThresholdPct = 96\.1') -and ($cgp -match 'never lower it') -and
  ($cgp -match 'Config\.ToLower') -and ($cgp -match '\\tests\\phantombt-tests')) `
  'gate assertions changed; re-audit tools/coverage-gate.ps1:43-62'

# ---- G4. run-m0..m3 real-device legs: any manual `usbip attach` must target the
# port serve actually listens on (param default) and must not use `-p` (usbip-win2
# attach has no -p; the TCP port is a global -t/--tcp-port before the subcommand).
foreach ($m in @('m0', 'm1', 'm2', 'm3')) {
  $mt = Get-Content (Join-Path $root ("tools/run-$m.ps1")) -Raw
  $servePort = $null
  if ($mt -match '\[int\]\$Port\s*=\s*(\d+)') { $servePort = $Matches[1] }
  $attachLines = @($mt -split "`r?`n" | Where-Object { $_ -match 'attach\s+-r' })
  if ($attachLines.Count -eq 0) {
    Check ("run-$m-attach-consistency") $true ('serve --attach path (in-process), serve port default ' + $servePort)
  } else {
    $line = ($attachLines -join ' | ').Trim()
    $bad = @()
    if ($line -match '\s-p\s+\d+') { $bad += 'attach uses -p (usbip-win2 attach has no -p option; port must be global -t/--tcp-port)' }
    foreach ($mm in [regex]::Matches($line, '(?:-t|--tcp-port|-p|--port)\s+(\d+)')) {
      if ($servePort -and ($mm.Groups[1].Value -ne $servePort)) {
        $bad += ('attach port ' + $mm.Groups[1].Value + ' != serve --port default ' + $servePort)
      }
    }
    Check ("run-$m-attach-consistency") ($bad.Count -eq 0) (($bad -join '; ') + ' [known-defect: this real-device leg cannot work as written]')
  }
}

# ---- G5. run-m3 probe address vs profiles/ble_keyboard.json (probe address is the
# little-endian byte order of the profile address: 5544332211C0 <-> C0:11:22:33:44:55).
$m3t = Get-Content (Join-Path $root 'tools/run-m3.ps1') -Raw
$profAddr = $null
try { $profAddr = (Get-Content (Join-Path $root 'profiles/ble_keyboard.json') -Raw | ConvertFrom-Json).address } catch {}
$addrOk = $false; $addrDetail = 'no --address <12hex> in run-m3.ps1 or no address in profiles/ble_keyboard.json'
if ($m3t -match '--address\s+([0-9A-Fa-f]{12})' -and $profAddr) {
  $addr = $Matches[1].ToUpper()
  $b = @([regex]::Matches($addr, '..') | ForEach-Object { $_.Value })
  [array]::Reverse($b)
  $rev = ($b -join '')
  $profDigits = ($profAddr -replace ':', '').ToUpper()
  $addrOk = ($rev -eq $profDigits)
  $addrDetail = ('probe ' + $addr + ' vs reversed profile ' + $profDigits)
}
Check 'run-m3-ble-address-matches-profile' $addrOk $addrDetail

# ---- G6. sys-bt-check.ps1 must have a failure path [known-defect]: today it writes
# sysbt-cli.txt unconditionally, prints 'done' and exits 0 whatever the pipe
# replied; its header even says it drives a "live GUI" while line 6 launches
# 'serve --load-bt'. (Its happy path -- `cmd "sys-bt list"` over the pipe -- is
# already asserted live by sysbt-list-line-format-over-pipe above.)
$sbt = Get-Content (Join-Path $root 'tools/sys-bt-check.ps1') -Raw
Check 'sys-bt-check-fails-on-error' ($sbt -match 'exit 1|throw ') `
  'no assertion/failure path: unconditionally writes sysbt-cli.txt + exit 0; header comment (live GUI) != behavior (serve --load-bt, line 6) [known-defect, audit row]'

# ---- G7. gui-verify.ps1: portability [known-defect #52] + check wiring.
$gv = Get-Content (Join-Path $root 'tools/gui-verify.ps1') -Raw
Check 'gui-verify-path-portability' (($gv -notmatch 'C:/tjf') -and ($gv -notmatch 'C:\\tjf') -and
  ($gv -match '\$PSScriptRoot')) `
  'hardcoded machine path(s) C:/tjf/... (lines 7-8) instead of $PSScriptRoot; breaks on any other checkout [known-defect #52, docs/global-review-2026-11-01.md]'
Check 'gui-verify-structure' (($gv -match '0x202') -and ($gv -match 'exit 1') -and
  ($gv -match 'FAILED: ') -and ($gv -match 'taskkill')) `
  'check wiring changed; re-audit tools/gui-verify.ps1'

# ---- G8. cov_gaps.py on synthetic cobertura XML (fixtures generated below, so no
# dependency on a real coverage run). Machine-independent: the drive-relative
# filename prefix is derived from this checkout's actual location. Classes are
# hashtables @{ f = filename; l = @('lineno:hits', ...) } -- PS nested array
# literals flatten, strings/hashtables do not (learned the hard way: the first
# version emitted empty hits= attributes).
function New-CovXml([string]$path, [string[]]$sources, $classes) {
  $x = '<coverage line-rate="1.0" lines-covered="0" lines-valid="0"><sources>'
  foreach ($s in $sources) { $x += '<source>' + $s + '</source>' }
  $x += '</sources><packages><package><classes>'
  foreach ($c in $classes) {
    $x += '<class filename="' + $c.f + '"><lines>'
    foreach ($ls in $c.l) {
      $pp = $ls.Split(':')
      $x += '<line number="' + $pp[0] + '" hits="' + $pp[1] + '"/>'
    }
    $x += '</lines></class>'
  }
  $x += '</classes></package></packages></coverage>'
  Set-Content -Path $path -Value $x -Encoding UTF8
}
$repoAbs = (Resolve-Path $root).Path
$gDrive = $repoAbs.Substring(0, 2)
$gRepoRel = $repoAbs.Substring(3).Replace('\', '/')
$cgDir = Join-Path $env:TEMP 'check-tools-cov'
New-Item -ItemType Directory -Force -Path $cgDir | Out-Null
$covPy = Join-Path $root 'tools/cov_gaps.py'

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
  Skip 'cov-gaps-gate-contract' 'python not on PATH'
  Skip 'cov-gaps-excludes-foreign-and-excluded-dirs' 'python not on PATH'
  Skip 'ci-workflow-structure' 'python not on PATH'
  Skip 'make-icon-compiles' 'python not on PATH'
  Skip 'make-icon-renders' 'python not on PATH'
} else {
  # G8a. documented exit-code contract (cov_gaps.py docstring): 0 pass /
  # 1 below threshold + failing-file list / 2 bad input.
  $bad = @()
  $clsPass = @(@{ f = $gRepoRel + '/src/ble/aes.cpp'; l = @('10:1', '11:1') })
  $clsFail = @(@{ f = $gRepoRel + '/src/ble/aes.cpp'; l = @('10:1', '11:0', '12:0') })
  New-CovXml (Join-Path $cgDir 'pass.xml') @($gDrive) $clsPass
  New-CovXml (Join-Path $cgDir 'fail.xml') @($gDrive) $clsFail
  Set-Content -Path (Join-Path $cgDir 'broken.xml') -Value '<coverage><sources><source>C:</source></sources><packages>><<' -Encoding UTF8

  $o = (& python -I $covPy --xml (Join-Path $cgDir 'pass.xml') --out (Join-Path $cgDir 'o1.json') --threshold 50 2>$null) -join "`n"
  if (($LASTEXITCODE -ne 0) -or ($o -notmatch 'GATE: PASS')) { $bad += 'pass-case rc=' + $LASTEXITCODE }
  $o = (& python -I $covPy --xml (Join-Path $cgDir 'fail.xml') --out (Join-Path $cgDir 'o2.json') --threshold 90 2>$null) -join "`n"
  if (($LASTEXITCODE -ne 1) -or ($o -notmatch 'GATE: FAIL') -or ($o -notmatch 'src/ble/aes\.cpp')) { $bad += 'fail-case rc=' + $LASTEXITCODE }
  $null = & python -I $covPy --xml (Join-Path $cgDir 'missing.xml') --out (Join-Path $cgDir 'o3.json') 2>$null
  if ($LASTEXITCODE -ne 2) { $bad += 'missing-xml rc=' + $LASTEXITCODE }
  $null = & python -I $covPy --xml (Join-Path $cgDir 'broken.xml') --out (Join-Path $cgDir 'o4.json') 2>$null
  if ($LASTEXITCODE -ne 2) { $bad += 'unparsable-xml rc=' + $LASTEXITCODE }
  Check 'cov-gaps-gate-contract' ($bad.Count -eq 0) ($bad -join '; ')

  # G8b. defensive exclusion must hold for EVERY filename form OpenCppCoverage can
  # emit [known-defect]: with sources=['C:'] an ABSOLUTE-path SDK class is counted
  # as a project file (SRC-base gluing, cov_gaps.py:47-48), and with a src-rooted
  # <source> a drive-relative src/gui file is counted (mapped rel path no longer
  # starts with 'gui'). Both reproduced 2026-11-01; the real coverage.xml is
  # unaffected only because its 51 classes are drive-rel src/ files with
  # sources=['C:'].
  $clsA = @(
    @{ f = 'C:/Program Files/SDK/toolkit/xstring'; l = @('5:0', '6:0') },
    @{ f = $gRepoRel + '/src/gui/main.cpp';         l = @('1:0') },
    @{ f = $gRepoRel + '/src/cli/main.cpp';         l = @('1:0') },
    @{ f = $gRepoRel + '/src/probes/main.cpp';      l = @('1:0') },
    @{ f = $gRepoRel + '/src/ble/aes.cpp';          l = @('10:1', '11:1') }
  )
  $clsB = @(
    @{ f = $gRepoRel + '/src/gui/main.cpp'; l = @('1:0') },
    @{ f = $gRepoRel + '/src/ble/aes.cpp';  l = @('10:1') }
  )
  New-CovXml (Join-Path $cgDir 'exclA.xml') @($gDrive) $clsA
  New-CovXml (Join-Path $cgDir 'exclB.xml') @(($repoAbs.Replace('\', '/') + '/src')) $clsB
  $badB = @()
  foreach ($n in @('exclA', 'exclB')) {
    $null = & python -I $covPy --xml (Join-Path $cgDir ($n + '.xml')) --out (Join-Path $cgDir ($n + '.json')) 2>$null
    if (-not (Test-Path (Join-Path $cgDir ($n + '.json')))) { $badB += ($n + ': no report written, rc=' + $LASTEXITCODE); continue }
    $rep = Get-Content (Join-Path $cgDir ($n + '.json')) -Raw | ConvertFrom-Json
    foreach ($fx in $rep.files) {
      if ($fx.file -ne 'src/ble/aes.cpp') { $badB += ($n + ' counted ''' + $fx.file + '''') }
    }
  }
  Check 'cov-gaps-excludes-foreign-and-excluded-dirs' ($badB.Count -eq 0) `
    (($badB -join '; ') + ' [known-defect, cov_gaps.py:44-48 defensive exclusion ineffective for these forms]')

  # ---- G9. ci.yml structure: YAML parses + all 8 steps + always-upload anchor.
  # (No double quotes inside the -c script: PS 5.1 strips them when passing the
  # argument to a native exe, which python then parses as a NameError.)
  $ciPath = Join-Path $root '.github/workflows/ci.yml'
  if (-not (Test-Path $ciPath)) {
    Check 'ci-workflow-structure' $false '.github/workflows/ci.yml missing'
  } else {
    $null = & python -I -c 'import sys, yaml; yaml.safe_load(open(sys.argv[1]))' $ciPath 2>$null
    $yamlOk = ($LASTEXITCODE -eq 0)
    $ciTxt = Get-Content $ciPath -Raw
    $anchors = @('actions/checkout', 'setup-python', 'msvc-dev-cmd', 'cmake --preset',
                 'phantombt-tests.exe', 'opencppcoverage', 'coverage-gate.ps1',
                 'upload-artifact', 'if: always')
    $prob = @()
    if (-not $yamlOk) { $prob += 'yaml parse failed' }
    $prob += @($anchors | Where-Object { $ciTxt -notmatch [regex]::Escape($_) })
    Check 'ci-workflow-structure' ($prob.Count -eq 0) ($prob -join '; ')
  }

  # ---- G10. one-off tools. make_icon.py: compiles, and (with Pillow) renders the
  # .ico into a temp cwd (its own res/ subdir pre-created -- PIL does not mkdir);
  # the repo's own res/ is never touched.
  $null = & python -I -m py_compile (Join-Path $root 'tools/make_icon.py') 2>$null
  Check 'make-icon-compiles' ($LASTEXITCODE -eq 0) ('py_compile rc=' + $LASTEXITCODE)
  $null = & python -I -c 'import PIL' 2>$null
  if ($LASTEXITCODE -ne 0) {
    Skip 'make-icon-renders' 'Pillow not installed'
  } else {
    $iconDir = Join-Path $env:TEMP 'check-tools-icon'
    New-Item -ItemType Directory -Force -Path $iconDir | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $iconDir 'res') | Out-Null
    Push-Location $iconDir
    $null = & python -I (Join-Path $root 'tools/make_icon.py') 2>$null
    $iconRc = $LASTEXITCODE
    Pop-Location
    $ico = Join-Path $iconDir 'res\app.ico'
    Check 'make-icon-renders' (($iconRc -eq 0) -and (Test-Path $ico) -and ((Get-Item $ico -ErrorAction SilentlyContinue).Length -gt 0)) `
      ('rc=' + $iconRc + ' ico exists=' + (Test-Path $ico))
  }
}

# ---- G11. create-repo.bat: structure only. Executing it creates a public GitHub
# repo (needs gh auth + network) -- deliberately never run by automation.
$crb = Get-Content (Join-Path $root 'tools/create-repo.bat') -Raw
$readmeTxt = Get-Content (Join-Path $root 'README.md') -Raw
Check 'create-repo-bat-structure-only' (($crb -match 'gh repo create') -and ($crb -match 'gh auth status') -and
  ($crb -match 'exit /b 1') -and ($readmeTxt -match 'create-repo\.bat')) `
  'structure changed or README no longer references it (never executed: creates a public remote repo)'

# ---- G12. CI workflow must actually be committed so CI can ever run remotely
# [known-defect]: today the whole .github/ dir is untracked and no remote run exists.
try {
  $tracked = @(& git -C $root ls-files -- .github/ 2>$null)
  if ($LASTEXITCODE -ne 0) { Skip 'ci-workflows-committed' 'git unavailable' }
  elseif ($tracked.Count -gt 0) { Check 'ci-workflows-committed' $true ($tracked -join ', ') }
  else { Check 'ci-workflows-committed' $false '.github/ has 0 tracked files (git status shows ?? .github/): CI has never run remotely [known gap, audit row ci.yml]' }
} catch { Skip 'ci-workflows-committed' 'git unavailable' }

# ==================== H. sim-control real-path rows (round 1B batch B) ====================
# The three "real machine" control-command rows this script previously left
# manual (see the header note and section F's SAFETY comment): real usbip.exe +
# kernel vhci import of our own loopback serve (pipe `attach` is the same
# Simulator::cmd_attach the serve --attach flag calls, src/cli/main.cpp:86),
# real kernel unbind via `detach`, and the physical-radio disable/enable round
# trip (section F only checks list/bad-op shapes).
# Guards: usbip.exe missing / not elevated / pipe not owned by our serve ->
# per-item SKIP. detach-real proves the by-device detach fix on the real kernel:
# FOREIGN imports (other usbip benches on this machine -- observed live: a
# phantom-audio serve held a 1d50:6222 import on port 3240) must SURVIVE a
# detach; a regression to the old fixed `usbip detach --all` fails the item.
# sysbt-real restores the baseline radio state in finally{}. Validated
# 2026-11-01 against usbip-win2 usbip.exe 0.9.8.0 (usbip2_ude driver running,
# elevated shell, Intel USB radio).

# -SkipReal (round 2): opt out of the three real-machine H items for runs that
# must not touch the kernel or the physical radio (e.g. validating later
# sections of this file). Inner block keeps its original indentation.
if ($SkipReal) {
  Skip 'attach-real-kernel-loopback' '-SkipReal given (no kernel vhci import in this run)'
  Skip 'detach-real-kernel-unbind' '-SkipReal given'
  Skip 'sysbt-real-toggle-roundtrip' '-SkipReal given'
  Skip 'multidev-real-attach-and-per-device-detach' '-SkipReal given'
  Skip 'sysbt-list-excludes-live-virtual-adapter' '-SkipReal given'
} else {

$usbipExe = $null
$ucmd = Get-Command usbip.exe -ErrorAction SilentlyContinue
if ($ucmd) { $usbipExe = $ucmd.Source }
if (-not $usbipExe) {
  foreach ($pp in @("$env:ProgramFiles\USBip\usbip.exe", "${env:ProgramFiles(x86)}\USBip\usbip.exe")) {
    if (Test-Path $pp) { $usbipExe = $pp; break }
  }
}
$hAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
  ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# .NET runner for usbip.exe: no PowerShell stderr redirection (robust under
# $ErrorActionPreference='Stop'); args are simple tokens, no quoting needed.
function Run-Native([string]$file, [string[]]$nargs, [int]$timeoutMs = 30000) {
  if ($null -eq $nargs) { $nargs = @() }
  $npsi = New-Object System.Diagnostics.ProcessStartInfo
  $npsi.FileName = $file
  $npsi.Arguments = ($nargs -join ' ')
  $npsi.UseShellExecute = $false
  $npsi.RedirectStandardOutput = $true
  $npsi.RedirectStandardError = $true
  $npsi.CreateNoWindow = $true
  $np = [System.Diagnostics.Process]::Start($npsi)
  $noT = $np.StandardOutput.ReadToEndAsync()
  $neT = $np.StandardError.ReadToEndAsync()
  if (-not $np.WaitForExit($timeoutMs)) { try { $np.Kill() } catch {}; $np.WaitForExit() }
  $np.WaitForExit()
  return @{ Rc = $np.ExitCode; Out = $noT.Result; Err = $neT.Result }
}

# "Port NN: ..." header lines of `usbip.exe port` = the current kernel imports.
function Get-ImportHeaders {
  if (-not $usbipExe) { return @() }
  $nr = Run-Native $usbipExe @('port')
  return @($nr.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -match '^Port \d+:' })
}

# Hub port number of the import block pointing at OUR serve (marker line
# "usbip://127.0.0.1:<port>/7-1"), else -1.
function Find-OurImport([int]$serverPort) {
  if (-not $usbipExe) { return -1 }
  $nr = Run-Native $usbipExe @('port')
  $mark = [regex]::Escape(('usbip://127.0.0.1:{0}/7-1' -f $serverPort))
  $last = -1
  foreach ($nl in ($nr.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") })) {
    if ($nl -match '^Port (\d+):') { $last = [int]$Matches[1] }
    elseif ($last -ge 1 -and $nl -match $mark) { return $last }
  }
  return -1
}

# Same as Find-OurImport but for an arbitrary busid (H4 needs 7-1 AND 7-2).
function Find-OurImportBid([int]$serverPort, [string]$bid) {
  if (-not $usbipExe) { return -1 }
  $nr = Run-Native $usbipExe @('port')
  $mark = [regex]::Escape(('usbip://127.0.0.1:{0}/{1}' -f $serverPort, $bid))
  $last = -1
  foreach ($nl in ($nr.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") })) {
    if ($nl -match '^Port (\d+):') { $last = [int]$Matches[1] }
    elseif ($last -ge 1 -and $nl -match $mark) { return $last }
  }
  return -1
}

$hport = Get-FreePort
$hErr = Join-Path $env:TEMP 'pb-check-serve-h.err'
$serveH = Start-Process -FilePath $cli -ArgumentList 'serve', '--port', "$hport", '--load', 'profiles/gamepad.json', '--exit-after-ms', '240000' `
  -WorkingDirectory $root -RedirectStandardError $hErr -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$hPortActual = 0
if (Test-Path $hErr) {
  $hm = Select-String -Path $hErr -Pattern 'usbip server on port (\d+)' | Select-Object -First 1
  if ($hm) { $hPortActual = [int]$hm.Matches[0].Groups[1].Value }
}
$hOwn = $false
if ($hPortActual -gt 0 -and -not $serveH.HasExited) {
  $hst = Run-Cli @('cmd', 'status')
  if ($hst.Rc -eq 0 -and $hst.Out -match ('^OK usbip-port=' + $hPortActual + ' ')) { $hOwn = $true }
}

# ---- H1. attach-real: kernel vhci import of our own loopback serve; cleanup is
# a surgical `usbip detach -p <our hub port>` (never --all: other benches on
# this machine may hold live imports).
if (-not $hOwn) {
  Skip 'attach-real-kernel-loopback' 'control pipe not owned by our serve (foreign bench or serve failed)'
} elseif (-not $usbipExe) {
  Skip 'attach-real-kernel-loopback' 'usbip.exe not found (usbip-win2 not installed)'
} elseif (-not $hAdmin) {
  Skip 'attach-real-kernel-loopback' 'kernel attach requires elevation'
} else {
  $ha = Run-Cli @('cmd', 'attach 7-1') 120
  $hub = Find-OurImport $hPortActual
  if (($ha.Rc -eq 0) -and ($ha.Out -match '(?m)^OK attached 7-1') -and ($hub -ge 1)) {
    [void](Run-Native $usbipExe @('detach', '-p', "$hub"))
    $unbound = ((Find-OurImport $hPortActual) -lt 0)
    Check 'attach-real-kernel-loopback' $unbound ("hub port $hub imported from usbip://127.0.0.1:$hPortActual/7-1; unbound by detach -p $hub")
  } else {
    if ($hub -ge 1) { [void](Run-Native $usbipExe @('detach', '-p', "$hub")) }  # best effort
    Check 'attach-real-kernel-loopback' $false ("rc=$($ha.Rc) out='$($ha.Out.Trim())' err='$($ha.Err.Trim())' hub=$hub")
  }
}

# ---- H2. detach-real: cmd `detach` unbinds by device (usbip detach -p N, the
# bug detach-by-device fix in src/sim/simulator.cpp). Asserted on the real
# kernel: detaching a not-imported busid is a no-op ("not attached"); after a
# real attach our import disappears while FOREIGN imports (other usbip benches
# on this machine, e.g. a phantom-audio serve on port 3240) survive -- a
# regression back to the old `detach --all` fails this item.
if (-not $hOwn) {
  Skip 'detach-real-kernel-unbind' 'control pipe not owned by our serve'
} elseif (-not $usbipExe -or -not $hAdmin) {
  Skip 'detach-real-kernel-unbind' 'usbip.exe or elevation missing'
} else {
  $foreignBefore = @(Get-ImportHeaders).Count
  $hn0 = Run-Cli @('cmd', 'detach 7-1') 60
  $noopOk = ($hn0.Out -match '(?m)^OK not attached')
  $ha2 = Run-Cli @('cmd', 'attach 7-1') 120
  $hub2 = Find-OurImport $hPortActual
  if (($ha2.Out -match '(?m)^OK attached 7-1') -and ($hub2 -ge 1)) {
    $hd = Run-Cli @('cmd', 'detach 7-1') 120
    $oursGone = ((Find-OurImport $hPortActual) -lt 0)
    $cntAfter = @(Get-ImportHeaders).Count
    Check 'detach-real-kernel-unbind' ($noopOk -and ($hd.Out -match '(?m)^OK detached') -and $oursGone -and ($cntAfter -eq $foreignBefore)) `
      ("no-op=$noopOk; reply='$($hd.Out.Trim())'; ours unbound=$oursGone; imports $foreignBefore->$cntAfter (foreign preserved)")
  } else {
    if ($hub2 -ge 1) { [void](Run-Native $usbipExe @('detach', '-p', "$hub2")) }
    Check 'detach-real-kernel-unbind' $false ("setup attach failed: rc=$($ha2.Rc) out='$($ha2.Out.Trim())' hub=$hub2")
  }
}

# ---- H3. sysbt-real: physical radio disable/enable round trip over the pipe,
# verified by the [disabled]/[enabled] prefixes of `sys-bt list`; baseline state
# restored in finally{} no matter what happens above.
if (-not $hOwn) {
  Skip 'sysbt-real-toggle-roundtrip' 'control pipe not owned by our serve'
} elseif (-not $hAdmin) {
  Skip 'sysbt-real-toggle-roundtrip' 'radio state change requires elevation'
} else {
  function Get-RadioRows {
    $xr = Run-Cli @('cmd', 'sys-bt list') 30
    return @($xr.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -match '^\[(enabled|disabled)\]\s+RADIO' })
  }
  $hRows = Get-RadioRows
  if ($hRows.Count -eq 0) {
    Skip 'sysbt-real-toggle-roundtrip' 'no physical bluetooth radio on this machine'
  } else {
    $wasEnabled = @($hRows | Where-Object { $_ -match '^\[enabled\]' }).Count -gt 0
    $hn = $hRows.Count
    $hOk2 = $true; $hWhy = ''
    try {
      if ($wasEnabled) {
        $d = Run-Cli @('cmd', 'sys-bt disable') 60
        $disN = @((Get-RadioRows) | Where-Object { $_ -match '^\[disabled\]' }).Count
        if (-not ($d.Out -match '(?m)^disabled \d+ radio')) { $hOk2 = $false; $hWhy = 'disable reply: ' + $d.Out.Trim() }
        elseif ($disN -ne $hn) { $hOk2 = $false; $hWhy = "after disable: $disN/$hn radios [disabled]" }
        $e = Run-Cli @('cmd', 'sys-bt enable') 60
        $enN = @((Get-RadioRows) | Where-Object { $_ -match '^\[enabled\]' }).Count
        if (-not ($e.Out -match '(?m)^enabled \d+ radio')) { $hOk2 = $false; $hWhy = 'enable reply: ' + $e.Out.Trim() }
        elseif ($enN -ne $hn) { $hOk2 = $false; $hWhy = "after enable: $enN/$hn radios [enabled]" }
      } else {
        $e = Run-Cli @('cmd', 'sys-bt enable') 60
        $enN = @((Get-RadioRows) | Where-Object { $_ -match '^\[enabled\]' }).Count
        if (-not ($e.Out -match '(?m)^enabled \d+ radio')) { $hOk2 = $false; $hWhy = 'enable reply: ' + $e.Out.Trim() }
        elseif ($enN -ne $hn) { $hOk2 = $false; $hWhy = "after enable: $enN/$hn radios [enabled]" }
        $d = Run-Cli @('cmd', 'sys-bt disable') 60
        $disN = @((Get-RadioRows) | Where-Object { $_ -match '^\[disabled\]' }).Count
        if (-not ($d.Out -match '(?m)^disabled \d+ radio')) { $hOk2 = $false; $hWhy = 'disable reply: ' + $d.Out.Trim() }
        elseif ($disN -ne $hn) { $hOk2 = $false; $hWhy = "after disable: $disN/$hn radios [disabled]" }
      }
    } finally {
      try {
        $nowRows = Get-RadioRows
        $nowEnabled = @($nowRows | Where-Object { $_ -match '^\[enabled\]' }).Count -gt 0
        if ($nowEnabled -ne $wasEnabled) {
          [void](Run-Cli @('cmd', ("sys-bt " + $(if ($wasEnabled) { 'enable' } else { 'disable' }))) 60)
        }
      } catch {}
    }
    Check 'sysbt-real-toggle-roundtrip' $hOk2 $hWhy
  }
}

# ---- H4. multi-device real kernel attach (inventory row: concurrent attach,
# real leg). Reuses the section-H serve (gamepad on 7-1): load the virtual BT
# adapter over the pipe (fresh serve -> next busid 7-2), attach BOTH busids
# through the same Simulator::cmd_attach path the serve --attach flag uses,
# then detach each by busid -- after `detach 7-1` the 7-2 import MUST survive
# (per-device detach; a regression to `usbip detach --all` fails this), and
# `detach 7-2` clears the rest. Final sweep detaches any of our imports that
# are still bound, on every path, so nothing of ours outlives the check.
if ($SkipReal) {
  Skip 'multidev-real-attach-and-per-device-detach' '-SkipReal given (no kernel vhci import in this run)'
} elseif (-not $hOwn) {
  Skip 'multidev-real-attach-and-per-device-detach' 'control pipe not owned by our serve (or section H was skipped)'
} elseif (-not $usbipExe -or -not $hAdmin) {
  Skip 'multidev-real-attach-and-per-device-detach' 'usbip.exe or elevation missing'
} else {
  function Find-HubWithRetry([int]$serverPort, [string]$bid) {
    $hp = -1
    for ($i = 0; $i -lt 5; $i++) {
      $hp = Find-OurImportBid $serverPort $bid
      if ($hp -ge 1) { break }
      Start-Sleep -Milliseconds 500
    }
    return $hp
  }
  $md = @()
  $lb = Run-Cli @('cmd', 'load-bt') 60
  if (-not ($lb.Out -match '(?m)^OK 7-\d+')) { $md += 'load-bt over pipe: ' + $lb.Out.Trim() }
  $a1 = Run-Cli @('cmd', 'attach 7-1') 120
  $a2 = Run-Cli @('cmd', 'attach 7-2') 120
  if (-not ($a1.Out -match '(?m)^OK attached 7-1')) { $md += 'attach 7-1: ' + $a1.Out.Trim() }
  if (-not ($a2.Out -match '(?m)^OK attached 7-2')) { $md += 'attach 7-2: ' + $a2.Out.Trim() }
  $hub1 = Find-HubWithRetry $hPortActual '7-1'
  $hub2 = Find-HubWithRetry $hPortActual '7-2'
  if ($hub1 -lt 1) { $md += 'no kernel import found for 7-1' }
  if ($hub2 -lt 1) { $md += 'no kernel import found for 7-2' }
  if ($md.Count -eq 0) {
    $d1 = Run-Cli @('cmd', 'detach 7-1') 120
    if (-not ($d1.Out -match '(?m)^OK detached')) { $md += 'detach 7-1 reply: ' + $d1.Out.Trim() }
    if ((Find-OurImportBid $hPortActual '7-1') -ge 1) { $md += '7-1 still imported after detach 7-1' }
    if ((Find-OurImportBid $hPortActual '7-2') -lt 1) { $md += '7-2 import did NOT survive detach 7-1 (detach --all regression?)' }
    $d2 = Run-Cli @('cmd', 'detach 7-2') 120
    if (-not ($d2.Out -match '(?m)^OK detached')) { $md += 'detach 7-2 reply: ' + $d2.Out.Trim() }
    if ((Find-OurImportBid $hPortActual '7-2') -ge 1) { $md += '7-2 still imported after detach 7-2' }
  }
  foreach ($bid in @('7-1', '7-2')) {   # best-effort cleanup on every path
    $hp = Find-OurImportBid $hPortActual $bid
    if ($hp -ge 1) { [void](Run-Native $usbipExe @('detach', '-p', "$hp")) }
  }
  Check 'multidev-real-attach-and-per-device-detach' ($md.Count -eq 0) `
    (($md -join '; ') + "  [hubs 7-1=$hub1 7-2=$hub2 while both attached]")
}

# ---- H5 (round 3B, bt_radio row): the VID_0B00 exclusion, live. While our
# virtual BT adapter is imported into the kernel it lands in the Bluetooth
# setup class (device/interface class codes 0xE0/01/01,
# src/bluetooth/adapter_model.cpp:63-67) with instance id
# USB\VID_0B00&PID_0001\... -- which ALSO passes is_physical_radio_id (USB\
# prefix, src/core/bt_radio.cpp:32-35). The exclusion at bt_radio.cpp:48 is
# therefore load-bearing: without it list_radios would mark our adapter
# physical=true and `sys-bt disable` would disable our own virtual adapter.
# Every earlier exclusion assertion (F above, tests/unit) is predicate-level or
# vacuously true while no VID_0B00 device exists in the class; this leg attaches
# the adapter FOR REAL and asserts the enumeration contract:
#   - `sys-bt list` framing stays OK/.../OK-END;
#   - no row contains VID_0B00 (the adapter, and any USB\VID_0B00...&MI_XX
#     interface children, must be dropped by the exclusion);
#   - the set of RADIO rows is byte-identical to the pre-attach baseline (the
#     Windows BT stack may legitimately add `child` rows for the new radio;
#     RADIO rows cannot legitimately change).
# Audit depth only (not PASS criteria): whether PnP actually surfaced a
# USB\VID_0B00* device in the Bluetooth class -- that is what proves the
# bt_radio.cpp:48 `continue` had a row to drop. Read-only for device state: NO
# sys-bt disable/enable runs here. The adapter is detached again on every path
# (pipe detach + surgical hub-port sweep, never --all).
if (-not $hOwn) {
  Skip 'sysbt-list-excludes-live-virtual-adapter' 'control pipe not owned by our serve'
} elseif (-not $usbipExe -or -not $hAdmin) {
  Skip 'sysbt-list-excludes-live-virtual-adapter' 'usbip.exe or elevation missing (kernel attach required)'
} else {
  # `sys-bt list` body rows between the OK / OK-END frame (Framed=$false on a
  # broken frame).
  function Read-SysBtList {
    $xr = Run-Cli @('cmd', 'sys-bt list') 30
    $rs = @($xr.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -ne '' })
    $body = @()
    if ($rs.Count -gt 2) { $body = @($rs[1..($rs.Count - 2)]) }
    $framed = ($rs.Count -ge 2 -and $rs[0] -eq 'OK' -and $rs[-1] -eq 'OK-END')
    return @{ Framed = $framed; Rows = $body }
  }
  $pre5 = Read-SysBtList
  # Find the virtual BT adapter on the bench (vid 0b00:0001); load it if absent.
  $btBid = ''
  foreach ($t5 in 1..2) {
    $ls5 = Run-Cli @('cmd', 'list') 30
    foreach ($ln in ($ls5.Out -split "`n")) {
      if ($ln.TrimEnd("`r") -match '^DEV (7-\d+) 0b00:') { $btBid = $Matches[1]; break }
    }
    if ($btBid) { break }
    [void](Run-Cli @('cmd', 'load-bt') 60)
  }
  if (-not $btBid) {
    Check 'sysbt-list-excludes-live-virtual-adapter' $false 'virtual BT adapter (0b00:) never appeared in cmd list'
  } elseif (-not $pre5.Framed) {
    Check 'sysbt-list-excludes-live-virtual-adapter' $false 'baseline sys-bt list framing broken'
  } else {
    $preRadio = @($pre5.Rows | Where-Object { $_ -match '^\[(enabled|disabled)\]\s+RADIO' })
    $att5 = Run-Cli @('cmd', ('attach ' + $btBid)) 120
    $hub5 = -1
    for ($i5 = 0; $i5 -lt 6; $i5++) {
      $hub5 = Find-OurImportBid $hPortActual $btBid
      if ($hub5 -ge 1) { break }
      Start-Sleep -Milliseconds 500
    }
    if ((-not ($att5.Out -match ('(?m)^OK attached ' + $btBid))) -or ($hub5 -lt 1)) {
      if ($hub5 -ge 1) { [void](Run-Native $usbipExe @('detach', '-p', "$hub5")) }
      Check 'sysbt-list-excludes-live-virtual-adapter' $false ("attach failed: rc=$($att5.Rc) out='$($att5.Out.Trim())' hub=$hub5")
    } else {
      # Give PnP a chance to surface the imported device (audit info only): if
      # the PnpDevice cmdlet exists, wait until a USB\VID_0B00* device shows up
      # and note whether Windows classed it as Bluetooth.
      $pnpOk = $null -ne (Get-Command Get-PnpDevice -ErrorAction SilentlyContinue)
      $btClass = $false
      if ($pnpOk) {
        foreach ($i5 in 1..16) {
          try {
            $d5 = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
              Where-Object { $_.InstanceId -like 'USB\VID_0B00*' })
            if ($d5.Count -gt 0) { $btClass = ($d5[0].Class -eq 'Bluetooth'); break }
          } catch {}
          Start-Sleep -Milliseconds 500
        }
      }
      $dur5 = Read-SysBtList
      # Cleanup BEFORE asserting, on every path: pipe detach + hub-port sweep.
      [void](Run-Cli @('cmd', ('detach ' + $btBid)) 120)
      $hp5 = Find-OurImportBid $hPortActual $btBid
      if ($hp5 -ge 1) { [void](Run-Native $usbipExe @('detach', '-p', "$hp5")) }
      $vidRows = @($dur5.Rows | Where-Object { $_ -match 'VID_0B00' })
      $durRadio = @($dur5.Rows | Where-Object { $_ -match '^\[(enabled|disabled)\]\s+RADIO' })
      $radioSame = (($preRadio -join '|') -eq ($durRadio -join '|'))
      $ok5 = ($dur5.Framed) -and ($vidRows.Count -eq 0) -and $radioSame
      Check 'sysbt-list-excludes-live-virtual-adapter' $ok5 `
        ("bid=$btBid hub=$hub5 btClassDevice=$btClass pnpCmdlet=$pnpOk vidRows=$($vidRows.Count) radioRows=$($durRadio.Count) baselineRadio=$($preRadio.Count)")
    }
  }
}

if ($serveH -and -not $serveH.HasExited) { Stop-Process -Id $serveH.Id -Force -ErrorAction SilentlyContinue }
Wait-Process -Id $serveH.Id -Timeout 10 -ErrorAction SilentlyContinue
}  # end -not $SkipReal (section H body; indentation inside intentionally kept)

# ==================== I. round-2 completion of the entry rows ====================
# I1 (rows: serve flag combos + cmd pipe client): exe-level multi-load. Round 1's
# cli-serve-all-flags-exit0 used ONE of each flag; here a serve is started with
# TWO --load flags and accumulation is verified through the pipe (7-1 vendor
# 1209:b00d AND 7-2 gamepad 1209:b007, in order), then the `cmd` client runs the
# session round 1 only ran against the GUI bench: inject -> status devices=2 ->
# unload-all -> status devices=0.
$iport = Get-FreePort
$iErrF = Join-Path $env:TEMP 'pb-check-serve-i.err'
$serveI = Start-Process -FilePath $cli -ArgumentList 'serve', '--port', "$iport", '--load', 'profiles/vendor.json', '--load', 'profiles/gamepad.json', '--exit-after-ms', '45000' `
  -WorkingDirectory $root -RedirectStandardError $iErrF -PassThru -WindowStyle Hidden
$iUp = $false
foreach ($i in 1..20) {
  Start-Sleep -Milliseconds 400
  if ($serveI.HasExited) { break }
  $it = Run-Cli @('cmd', 'status')
  if ($it.Rc -eq 0 -and $it.Out -match '^OK usbip-port=') { $iUp = $true; break }
}
if (-not $iUp) {
  $iErrTxt = ''
  if (Test-Path $iErrF) { $iErrTxt = (Get-Content $iErrF -Raw) }
  Check 'r2-serve-multi-load-accumulation' $false ('serve never answered on the pipe; err=' + $iErrTxt)
  Check 'r2-cmd-inject-unload-against-serve' $false 'bench not up'
} else {
  $ili = Run-Cli @('cmd', 'list')
  $accOk = (($ili.Out -match '(?m)^DEV 7-1 1209:b00d') -and ($ili.Out -match '(?m)^DEV 7-2 1209:b007'))
  Check 'r2-serve-multi-load-accumulation' ($ili.Rc -eq 0 -and $accOk) ('out=' + (($ili.Out -replace "`r`n", ' | ').Trim()))

  $iinj = Run-Cli @('cmd', 'inject 7-1 c0ffee')
  $ist = Run-Cli @('cmd', 'status')
  $iun = Run-Cli @('cmd', 'unload-all')
  $ist2 = Run-Cli @('cmd', 'status')
  Check 'r2-cmd-inject-unload-against-serve' `
    (($iinj.Out -match '(?m)^OK injected 3 bytes') -and ($ist.Out -match 'devices=2') -and
     ($iun.Out -match '(?m)^OK unloaded 2 device') -and ($ist2.Out -match 'devices=0')) `
    ('inj=' + $iinj.Out.Trim() + ' | st=' + $ist.Out.Trim() + ' | un=' + $iun.Out.Trim() + ' | st2=' + $ist2.Out.Trim())
}
if ($serveI -and -not $serveI.HasExited) { Stop-Process -Id $serveI.Id -Force -ErrorAction SilentlyContinue }
Wait-Process -Id $serveI.Id -Timeout 10 -ErrorAction SilentlyContinue

function Read-TcpExact([System.IO.Stream]$stm, [int]$n) {
  $buf = New-Object byte[] $n
  $got = 0
  while ($got -lt $n) {
    $r = $stm.Read($buf, $got, $n - $got)
    if ($r -le 0) { throw ('short read: ' + $got + '/' + $n) }
    $got += $r
  }
  return ,$buf
}

# I2..I6 (rows: GUI pages 0-2 buttons + embedded usbip/pipe): fresh GUI instance.
# 101/201 open GetOpenFileNameW modally on the UI thread, so the click is POSTed
# (async) and the owned #32770 dialog is driven directly (see header note).
if ($SkipGui) {
  Skip 'r2-gui-load-hid-dialog-button' '-SkipGui given (no interactive desktop)'
  Skip 'r2-gui-usbip-devlist-wire' '-SkipGui given'
  Skip 'r2-gui-inject-with-selection' '-SkipGui given'
  Skip 'r2-gui-ble-dialog-button' '-SkipGui given'
  Skip 'r2-gui-ble-stop-with-selection' '-SkipGui given'
} elseif (-not (Test-Path $gui)) {
  Skip 'r2-gui-load-hid-dialog-button' ('phantombt-gui.exe not found at ' + $gui)
  Skip 'r2-gui-usbip-devlist-wire' 'gui exe missing'
  Skip 'r2-gui-inject-with-selection' 'gui exe missing'
  Skip 'r2-gui-ble-dialog-button' 'gui exe missing'
  Skip 'r2-gui-ble-stop-with-selection' 'gui exe missing'
} else {
  $ksig = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections;
public class K {
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, string lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageSB(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr FindByTitlePart(string part) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      if (IsWindowVisible(h) && found == IntPtr.Zero && GetWindowTextLength(h) > 0) {
        var sb = new StringBuilder(256); GetWindowText(h, sb, sb.Capacity);
        if (sb.ToString().IndexOf(part) >= 0) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static ArrayList Descendants(IntPtr root) {
    var l = new ArrayList();
    EnumChildWindows(root, (h, lp) => { l.Add(h); return true; }, IntPtr.Zero);
    return l;
  }
  public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
  public static IntPtr ChildById(IntPtr parent, int id) {
    foreach (var d in Descendants(parent)) { IntPtr h = (IntPtr)d; if (GetDlgCtrlID(h) == id) return h; }
    return IntPtr.Zero;
  }
  public static IntPtr ChildByClass(IntPtr parent, string cls) {
    foreach (var d in Descendants(parent)) { IntPtr h = (IntPtr)d; if (ClassOf(h) == cls) return h; }
    return IntPtr.Zero;
  }
  public static IntPtr FindOwnedDialog(IntPtr owner) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, lp) => {
      if (found == IntPtr.Zero && IsWindowVisible(h) && GetWindow(h, 4) == owner && ClassOf(h) == "#32770") { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static IntPtr FilenameEdit(IntPtr dlg) {
    IntPtr combo = IntPtr.Zero;
    foreach (var d in Descendants(dlg)) { IntPtr h = (IntPtr)d; if (GetDlgCtrlID(h) == 1148 && (ClassOf(h) == "ComboBoxEx32" || ClassOf(h) == "ComboBox")) { combo = h; break; } }
    if (combo != IntPtr.Zero) foreach (var d in Descendants(combo)) { IntPtr h = (IntPtr)d; if (ClassOf(h) == "Edit") return h; }
    foreach (var d in Descendants(dlg)) { IntPtr h = (IntPtr)d; if (ClassOf(h) == "Edit" && GetDlgCtrlID(h) != 1148) return h; }
    return IntPtr.Zero;
  }
  public static void SetTextW(IntPtr h, string s) { SendMessageW(h, 0x000C, IntPtr.Zero, s); }
  public static string GetText(IntPtr h) {
    IntPtr len = SendMessageW(h, 0x000E, IntPtr.Zero, IntPtr.Zero);
    var sb = new StringBuilder(len.ToInt32() + 2);
    SendMessageSB(h, 0x000D, (IntPtr)(len.ToInt32() + 2), sb);
    return sb.ToString();
  }
  public static void DialogOk(IntPtr dlg) {
    IntPtr b = IntPtr.Zero;
    foreach (var d in Descendants(dlg)) { IntPtr h = (IntPtr)d; if (GetDlgCtrlID(h) == 1 && ClassOf(h) == "Button") { b = h; break; } }
    PostMessage(dlg, 0x0111, (IntPtr)1, b);  // WM_COMMAND IDOK
  }
  public static void DialogCancel(IntPtr dlg) { PostMessage(dlg, 0x0111, (IntPtr)2, IntPtr.Zero); }
  public static void Click(IntPtr h) {
    RECT r; GetWindowRect(h, out r);
    int cx = (r.L + r.R) / 2 - r.L, cy = (r.T + r.B) / 2 - r.T;
    SendMessageW(h, 0x202, IntPtr.Zero, (IntPtr)((((cy & 0xFFFF)) << 16) | (cx & 0xFFFF)));
  }
  public static void PostClick(IntPtr h) {
    RECT r; GetWindowRect(h, out r);
    int cx = (r.L + r.R) / 2 - r.L, cy = (r.T + r.B) / 2 - r.T;
    PostMessage(h, 0x202, IntPtr.Zero, (IntPtr)((((cy & 0xFFFF)) << 16) | (cx & 0xFFFF)));
  }
  public static void SelectListItem(IntPtr lv, int idx) {
    // CROSS-PROCESS CAUTION: LVM_SETITEMSTATE takes an LVITEM POINTER and
    // user32 does not marshal pointer-to-struct messages across processes --
    // sending it from here makes the GUI read a foreign address and die
    // (reproduced: GUI process dead immediately after the call). Only the
    // value-only LVM_SETSELECTIONMARK is sent -- exactly what selected_busid()
    // reads back (ListView_GetSelectionMark, src/gui/main.cpp:287).
    SendMessageW(lv, 0x1043, IntPtr.Zero, (IntPtr)idx);   // LVM_SETSELECTIONMARK
  }
}
'@
  Add-Type -TypeDefinition $ksig

  function Read-KLog([IntPtr]$mw) {
    $page3 = [K]::ChildById($mw, 3)
    if ($page3 -eq [IntPtr]::Zero) { return '' }
    $e = [K]::ChildByClass($page3, 'Edit')
    if ($e -eq [IntPtr]::Zero) { return '' }
    return [K]::GetText($e)
  }

  # Click a button that opens GetOpenFileNameW, fill the dialog, press OK.
  # Returns 'ok' on success, a short reason otherwise (the dialog is cancelled
  # best-effort on the failure paths so the UI thread never stays stuck).
  function Invoke-PbFileDialog([IntPtr]$btn, [string]$path, [IntPtr]$owner) {
    [void][K]::PostClick($btn)
    $dlg = [IntPtr]::Zero
    foreach ($i in 1..16) {
      Start-Sleep -Milliseconds 500
      $dlg = [K]::FindOwnedDialog($owner)
      if ($dlg -ne [IntPtr]::Zero) { break }
    }
    if ($dlg -eq [IntPtr]::Zero) { return 'no-dialog' }
    $fe = [K]::FilenameEdit($dlg)
    if ($fe -eq [IntPtr]::Zero) { [void][K]::DialogCancel($dlg); return 'no-filename-edit' }
    [void][K]::SetTextW($fe, $path)
    [K]::DialogOk($dlg)
    foreach ($i in 1..16) {
      Start-Sleep -Milliseconds 500
      if ([K]::FindOwnedDialog($owner) -eq [IntPtr]::Zero) { return 'ok' }
    }
    [void][K]::DialogCancel($dlg)
    return 'dialog-did-not-close'
  }

  cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
  foreach ($i in 1..20) {   # pipe must be free or the GUI's PipeServer.start loses it
    Start-Sleep -Milliseconds 500
    if (-not (Test-PipeExists 'phantom-bt')) { break }
  }
  Start-Process -FilePath $gui -WorkingDirectory $root
  $mw2 = [IntPtr]::Zero
  foreach ($i in 1..20) {
    Start-Sleep -Milliseconds 500
    $gp = Get-Process phantombt-gui -ErrorAction SilentlyContinue
    if ($gp -and $gp.MainWindowHandle -ne 0) { $mw2 = $gp.MainWindowHandle; break }
  }
  $gport2 = 0
  if ($mw2 -ne [IntPtr]::Zero) {
    foreach ($i in 1..20) {
      $t2 = Run-Cli @('cmd', 'status')
      if ($t2.Rc -eq 0 -and $t2.Out -match 'usbip-port=(\d+)') { $gport2 = [int]$matches[1]; break }
      Start-Sleep -Milliseconds 500
    }
  }

  if ($mw2 -eq [IntPtr]::Zero -or $gport2 -eq 0) {
    Check 'r2-gui-load-hid-dialog-button' $false ('main window found=' + ($mw2 -ne [IntPtr]::Zero) + ' pipe port=' + $gport2)
    Check 'r2-gui-usbip-devlist-wire' $false 'bench not up'
    Check 'r2-gui-inject-with-selection' $false 'bench not up'
    Check 'r2-gui-ble-dialog-button' $false 'bench not up'
    Check 'r2-gui-ble-stop-with-selection' $false 'bench not up'
  } else {
    # --- I2: "load HID device" (101) through the real file dialog
    $pageDev2 = [K]::ChildById($mw2, 1)
    $btn101 = [K]::ChildById($pageDev2, 101)
    $hidOk = $false; $hidWhy = ''
    if ($btn101 -eq [IntPtr]::Zero) {
      $hidWhy = 'button 101 not found on device page'
    } else {
      $dlgRc = Invoke-PbFileDialog $btn101 (Join-Path $root 'profiles\vendor.json') $mw2
      if ($dlgRc -ne 'ok') {
        $hidWhy = 'dialog automation: ' + $dlgRc
      } else {
        $hidSeen = $false
        foreach ($i in 1..10) {
          $l2 = Run-Cli @('cmd', 'list')
          if ($l2.Out -match '(?m)^DEV 7-1 1209:b00d') { $hidSeen = $true; break }
          Start-Sleep -Milliseconds 500
        }
        $hidLog = (Read-KLog $mw2) -match '> load-hid'
        if (-not $hidSeen) { $hidWhy = 'device 7-1 1209:b00d never appeared via cmd list' }
        elseif (-not $hidLog) { $hidWhy = 'device loaded but log page has no "> load-hid" line' }
        else { $hidOk = $true }
      }
    }
    Check 'r2-gui-load-hid-dialog-button' $hidOk $hidWhy

    # --- I3: wire-level OP_REQ_DEVLIST against the GUI's embedded usbip server.
    # Layout per src/usbip/protocol.cpp: 8B op_common + u32 ndev, then per device
    # 312B (path[256], busid[32] @256, busnum/devnum/speed u32, vid u16be @300,
    # pid u16be @302, ..., num_interfaces u8 @311) + num_interfaces*4B entries.
    $devOk = $false; $devWhy = 'devlist not attempted'
    if (-not $hidOk) {
      $devWhy = 'load-hid dialog step failed; devlist preconditions absent'
    } else {
      try {
        $tc2 = New-Object System.Net.Sockets.TcpClient
        if ($tc2.ConnectAsync('127.0.0.1', $gport2).Wait(2000)) {
          $st2 = $tc2.GetStream()
          $req2 = [byte[]](0x01, 0x11, 0x80, 0x05, 0, 0, 0, 0)   # version 0x0111 + OP_REQ_DEVLIST + status 0
          $st2.Write($req2, 0, 8)
          $st2.Flush()
          $hdr2 = Read-TcpExact $st2 12
          # NB: cast each byte to int BEFORE -shl -- in PS 5.1 [byte] -shl N
          # truncates the result back to byte ([byte]0x01 -shl 8 == 0), which
          # turned version 0x0111 into 0x0011 in the first version of this check.
          $ver2 = (([int]$hdr2[0] -shl 8) -bor [int]$hdr2[1])
          $code2 = (([int]$hdr2[2] -shl 8) -bor [int]$hdr2[3])
          $ndev2 = (([int]$hdr2[8] -shl 24) -bor ([int]$hdr2[9] -shl 16) -bor ([int]$hdr2[10] -shl 8) -bor [int]$hdr2[11])
          if ($ver2 -ne 0x0111 -or $code2 -ne 0x0005) {
            $devWhy = ('reply version=0x{0:X4} code=0x{1:X4} (want 0x0111/0x0005)' -f $ver2, $code2)
          } elseif ($ndev2 -ne 1) {
            $devWhy = ('ndev=' + $ndev2 + ' (want 1: the device loaded via the dialog)')
          } else {
            $blk2 = Read-TcpExact $st2 312
            $nif2 = $blk2[311]
            [void](Read-TcpExact $st2 ($nif2 * 4))
            $busid2 = ([System.Text.Encoding]::ASCII.GetString($blk2, 256, 31)).Trim([char]0)
            $vid2 = (([int]$blk2[300] -shl 8) -bor [int]$blk2[301])
            $pid2 = (([int]$blk2[302] -shl 8) -bor [int]$blk2[303])
            $devOk = (($busid2 -eq '7-1') -and ($vid2 -eq 0x1209) -and ($pid2 -eq 0xB00D))
            $devWhy = ('busid=' + $busid2 + ' vid=0x' + ('{0:X4}' -f $vid2) + ' pid=0x' + ('{0:X4}' -f $pid2) + ' interfaces=' + $nif2)
          }
        } else { $devWhy = 'tcp connect to the embedded server timed out' }
        $tc2.Close()
      } catch { $devWhy = 'exception: ' + $_.Exception.Message }
    }
    Check 'r2-gui-usbip-devlist-wire' $devOk $devWhy

    # --- I4: inject (106) with a selected listview row + hex read from edit 105
    $injOk = $false; $injWhy = ''
    if (-not $hidOk) {
      $injWhy = 'precondition: load-hid dialog step failed'
    } else {
      $lv2 = [K]::ChildByClass($pageDev2, 'SysListView32')
      $edit105 = [K]::ChildByClass($pageDev2, 'Edit')
      $btn106 = [K]::ChildById($pageDev2, 106)
      if ($lv2 -eq [IntPtr]::Zero -or $edit105 -eq [IntPtr]::Zero -or $btn106 -eq [IntPtr]::Zero) {
        $injWhy = 'listview/inject-edit/button 106 not found'
      } else {
        [K]::SelectListItem($lv2, 0)
        [void][K]::SetTextW($edit105, 'c0ffee')
        [K]::Click($btn106)   # non-modal handler: synchronous click is safe here
        Start-Sleep -Milliseconds 600
        $ilog2 = Read-KLog $mw2
        $injOk = (($ilog2 -match '> inject 7-1 c0ffee') -and ($ilog2 -match 'OK injected 3 bytes'))
        if (-not $injOk) { $injWhy = 'log page missing "> inject 7-1 c0ffee" or "OK injected 3 bytes"' }
      }
    }
    Check 'r2-gui-inject-with-selection' $injOk $injWhy

    # --- I5: "load BLE advertise" (201) through the real file dialog
    $bleOk = $false; $bleWhy = ''
    $pageBle2 = [K]::ChildById($mw2, 2)
    $btn201 = [K]::ChildById($pageBle2, 201)
    if ($btn201 -eq [IntPtr]::Zero) {
      $bleWhy = 'button 201 not found on BLE page'
    } else {
      [void](Run-Cli @('cmd', 'load-bt'))   # ble-advertise needs a BT adapter on the bench
      $dlgRc2 = Invoke-PbFileDialog $btn201 (Join-Path $root 'profiles\ble_keyboard.json') $mw2
      if ($dlgRc2 -ne 'ok') {
        $bleWhy = 'dialog automation: ' + $dlgRc2
      } else {
        $bleSeen = $false
        foreach ($i in 1..10) {
          $bl2 = Run-Cli @('cmd', 'ble-list')
          if ($bl2.Out -match 'C0:11:22:33:44:55') { $bleSeen = $true; break }
          Start-Sleep -Milliseconds 500
        }
        if ($bleSeen) { $bleOk = $true } else { $bleWhy = 'C0:11:22:33:44:55 never appeared in cmd ble-list' }
      }
    }
    Check 'r2-gui-ble-dialog-button' $bleOk $bleWhy

    # --- I6: ble-stop (202) with a real listbox selection
    $stOk = $false; $stWhy = ''
    if (-not $bleOk) {
      $stWhy = 'precondition: advertise dialog step failed'
    } else {
      $lb2 = [K]::ChildByClass($pageBle2, 'ListBox')
      $btn202 = [K]::ChildById($pageBle2, 202)
      if ($lb2 -eq [IntPtr]::Zero -or $btn202 -eq [IntPtr]::Zero) {
        $stWhy = 'listbox/button 202 not found'
      } else {
        [void][K]::SendMessageW($lb2, 0x186, [IntPtr]::Zero, [IntPtr]::Zero)   # LB_SETCURSEL item 0
        [K]::Click($btn202)
        $gone = $false
        foreach ($i in 1..10) {
          $bl3 = Run-Cli @('cmd', 'ble-list')
          if ($bl3.Out -notmatch 'C0:11:22:33:44:55') { $gone = $true; break }
          Start-Sleep -Milliseconds 500
        }
        if ($gone) { $stOk = $true } else { $stWhy = 'address still advertising after the ble-stop click' }
      }
    }
    Check 'r2-gui-ble-stop-with-selection' $stOk $stWhy

    # --- shutdown (same contract as section E)
    [void][K]::PostMessage($mw2, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
    foreach ($i in 1..20) {
      Start-Sleep -Milliseconds 500
      if ($null -eq (Get-Process phantombt-gui -ErrorAction SilentlyContinue)) { break }
    }
  }
  cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
}

# ==================== J. README acceptance matrix vs result artifacts (round 2B) ====================
# Inventory row "README acceptance matrix out of sync with real-device status":
# the README matrix must not contradict the persisted run artifacts
# integration-results/m{0..3}.json. Rules: a milestone whose artifact says
# realdevice PASS* must no longer claim the driver-pending state in README
# (matched via \uXXXX escapes to keep this file ASCII-only); an artifact saying
# FAIL* must not have README claiming more than pending. Artifacts saying SKIP
# assert nothing (that leg simply did not run in the batch that wrote them).
# [known-defect today: m0.json realdevice=PASS (2026-11-01 11:39 batch) while
# README.md:78 still claims the M0 true-machine leg awaits a driver]
$readmeAll = Get-Content (Join-Path $root 'README.md') -Raw -Encoding UTF8  # CJK rows; PS 5.1 default would garble them
$syncBad = @()
foreach ($m in @('m0', 'm1', 'm2', 'm3')) {
  $mj = Join-Path $root ("integration-results/" + $m + ".json")
  if (-not (Test-Path $mj)) { $syncBad += ($m + ': no integration-results/' + $m + '.json artifact'); continue }
  $rd = $null
  try { $rd = (Get-Content $mj -Raw | ConvertFrom-Json).realdevice } catch {}
  if ($null -eq $rd) { $syncBad += ($m + ': artifact has no realdevice field'); continue }
  if ($rd -notmatch '^(PASS|FAIL)') { continue }
  $row = @($readmeAll -split "`r?`n" | Where-Object { $_ -match ('^\| ' + $m.ToUpperInvariant() + '\b') }) | Select-Object -First 1
  if (-not $row) { $syncBad += ($m + ': no README matrix row'); continue }
  $pending = ($row -match '\u5F85\u9A71\u52A8')   # CJK "driver pending", ASCII-safe
  if (($rd -like 'PASS*') -and $pending) { $syncBad += ($m + ': artifact realdevice=' + $rd + ' but README row still claims driver-pending') }
  if (($rd -like 'FAIL*') -and (-not $pending)) { $syncBad += ($m + ': artifact realdevice=' + $rd + ' but README row no longer claims driver-pending') }
}
Check 'readme-matrix-syncs-with-results' ($syncBad.Count -eq 0) ($syncBad -join '; ')

# ============ K. control-command rows, entry-level completion (round 3B) ============
# Three legs the previous rounds still left open on the same matrix rows:
#   K1 `serve --attach` END TO END: run_serve's real flag path (src/cli/
#       main.cpp:96-111) attaches the serve's OWN loopback usbip server into the
#       kernel at startup -- the same real import section H drives through the
#       pipe, but via the startup flag this file's header used to list as a
#       manual-only leg. Cleanup is a surgical `usbip detach -p <our hub port>`
#       (never --all: other benches on this machine may hold live imports).
#   K2 the serve --attach SKIP / FAILURE legs (no kernel state change):
#       usbip.exe missing -> "attach: usbip.exe not found ...; continuing without
#       attach" + rc 0 (pure software -- runs even under -SkipReal); usbip.exe
#       present but NOT elevated -> per-device "attach 7-1: ..." stderr line,
#       serve KEEPS serving (rc 0), no import left behind (this leg does run the
#       real usbip.exe client, so it honours -SkipReal like section H).
#   K3 GUI global-settings sys-bt buttons (nav 14 -> page container id 5,
#       buttons 401/402 -> run_and_show("sys-bt disable"/"enable"),
#       src/gui/main.cpp:310-316): gui-verify.ps1 only proves the GUI SURVIVES
#       the disable click; here the state round trip is asserted over the pipe
#       with baseline restore (same contract as H3), plus the "> sys-bt" command
#       echo on the log page.
# K1 reuses section H's Run-Native / Find-OurImportBid helpers ($usbipExe) and
# K3 reuses section E's [G] type / Find-ByCtrlId / Read-LogPage: both providers
# precede K in this file and share its gating, so they are defined by the time
# K runs (PowerShell executes top to bottom; if/else bodies do not scope away
# script functions or types).
$kUsbip = $null
$kcmd = Get-Command usbip.exe -ErrorAction SilentlyContinue
if ($kcmd) { $kUsbip = $kcmd.Source }
if (-not $kUsbip) {
  foreach ($pp in @("$env:ProgramFiles\USBip\usbip.exe", "${env:ProgramFiles(x86)}\USBip\usbip.exe")) {
    if (Test-Path $pp) { $kUsbip = $pp; break }
  }
}
$kAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
  ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# ---- K2a: usbip.exe missing -> explicit skip message, serve still exits 0.
if ($kUsbip) {
  Skip 'serve-attach-flag-no-usbip-continues' ('usbip.exe installed at ' + $kUsbip + ' -- skip leg unreachable')
} else {
  $k2a = Run-Cli @('serve', '--attach', '--load', 'profiles/vendor.json', '--exit-after-ms', '1200') 30
  Check 'serve-attach-flag-no-usbip-continues' `
    (($k2a.Rc -eq 0) -and
     ($k2a.Err -match 'attach: usbip\.exe not found \(install usbip-win2\); continuing without attach')) `
    ("rc=" + $k2a.Rc + " err='" + $k2a.Err.Trim() + "'")
}

# ---- K1: real kernel import via the serve --attach startup flag.
if ($SkipReal) {
  Skip 'serve-attach-flag-real-kernel-loopback' '-SkipReal given (no kernel vhci import in this run)'
} elseif (-not $kUsbip) {
  Skip 'serve-attach-flag-real-kernel-loopback' 'usbip.exe not found (usbip-win2 not installed)'
} elseif (-not $kAdmin) {
  Skip 'serve-attach-flag-real-kernel-loopback' 'kernel attach requires elevation'
} else {
  $k1port = Get-FreePort
  $k1ErrF = Join-Path $env:TEMP 'pb-check-serve-k1.err'
  $k1srv = Start-Process -FilePath $cli -ArgumentList 'serve', '--port', "$k1port", '--load', 'profiles/gamepad.json', '--attach', '--exit-after-ms', '25000' `
    -WorkingDirectory $root -RedirectStandardError $k1ErrF -PassThru -WindowStyle Hidden
  # serve retries 10 ports upward, so trust only the port it REPORTS (same as H)
  $k1actual = 0
  foreach ($i in 1..10) {
    Start-Sleep -Milliseconds 300
    if (Test-Path $k1ErrF) {
      $k1m = Select-String -Path $k1ErrF -Pattern 'usbip server on port (\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($k1m) { $k1actual = [int]$k1m.Matches[0].Groups[1].Value; break }
    }
    if ($k1srv.HasExited) { break }
  }
  if ($k1actual -eq 0) { $k1actual = $k1port }   # stderr unreadable: assume requested
  $k1hub = -1
  foreach ($i in 1..30) {   # attach fires right after the load; poll the kernel list
    if ($k1srv.HasExited) { break }
    $k1hub = Find-OurImportBid $k1actual '7-1'
    if ($k1hub -ge 1) { break }
    Start-Sleep -Milliseconds 500
  }
  $k1errTxt = ''
  if (Test-Path $k1ErrF) { $k1errTxt = Get-Content $k1ErrF -Raw }
  $k1ok = (($k1hub -ge 1) -and ($k1errTxt -notmatch 'attach 7-1: '))
  if ($k1hub -ge 1) { [void](Run-Native $kUsbip @('detach', '-p', "$k1hub")) }   # surgical cleanup
  $k1gone = ((Find-OurImportBid $k1actual '7-1') -lt 1)
  if ($k1srv -and -not $k1srv.HasExited) { Stop-Process -Id $k1srv.Id -Force -ErrorAction SilentlyContinue }
  Wait-Process -Id $k1srv.Id -Timeout 10 -ErrorAction SilentlyContinue
  Check 'serve-attach-flag-real-kernel-loopback' ($k1ok -and $k1gone) `
    ("hub=$k1hub unboundByCleanup=$k1gone; serve stderr='" + $k1errTxt.Trim() + "'")
}

# ---- K2b: usbip.exe present but unelevated -> per-device failure logged, serve
# continues (rc 0) and nothing stays imported.
if ($SkipReal) {
  Skip 'serve-attach-flag-failure-logged-serve-continues' '-SkipReal given (this leg runs the real usbip.exe client)'
} elseif (-not $kUsbip) {
  Skip 'serve-attach-flag-failure-logged-serve-continues' 'usbip.exe not found (failure leg needs the real client)'
} elseif ($kAdmin) {
  Skip 'serve-attach-flag-failure-logged-serve-continues' 'elevated shell: the flag succeeds here (see serve-attach-flag-real-kernel-loopback)'
} else {
  $k2port = Get-FreePort
  $k2ErrF = Join-Path $env:TEMP 'pb-check-serve-k2.err'
  $k2srv = Start-Process -FilePath $cli -ArgumentList 'serve', '--port', "$k2port", '--load', 'profiles/gamepad.json', '--attach', '--exit-after-ms', '6000' `
    -WorkingDirectory $root -RedirectStandardError $k2ErrF -PassThru -WindowStyle Hidden
  Wait-Process -Id $k2srv.Id -Timeout 30 -ErrorAction SilentlyContinue
  if (-not $k2srv.HasExited) { Stop-Process -Id $k2srv.Id -Force -ErrorAction SilentlyContinue; Wait-Process -Id $k2srv.Id -Timeout 10 -ErrorAction SilentlyContinue }
  $k2errTxt = ''
  if (Test-Path $k2ErrF) { $k2errTxt = Get-Content $k2ErrF -Raw }
  $k2actual = $k2port   # serve may have moved up from a clash; trust its report
  $k2m = Select-String -Path $k2ErrF -Pattern 'usbip server on port (\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($k2m) { $k2actual = [int]$k2m.Matches[0].Groups[1].Value }
  $k2noImport = ((Find-OurImportBid $k2actual '7-1') -lt 1)
  Check 'serve-attach-flag-failure-logged-serve-continues' `
    (($k2srv.ExitCode -eq 0) -and ($k2errTxt -match 'attach 7-1: ') -and $k2noImport) `
    ("rc=" + $k2srv.ExitCode + " noImport=" + $k2noImport + " err='" + $k2errTxt.Trim() + "'")
}

# ---- K3: GUI global-settings sys-bt buttons drive a real radio round trip.
if ($SkipReal) {
  Skip 'gui-settings-sysbt-buttons-real-toggle' '-SkipReal given (no physical radio state change in this run)'
} elseif ($SkipGui) {
  Skip 'gui-settings-sysbt-buttons-real-toggle' '-SkipGui given (no interactive desktop in this environment)'
} elseif (-not (Test-Path $gui)) {
  Skip 'gui-settings-sysbt-buttons-real-toggle' ('phantombt-gui.exe not found at ' + $gui)
} elseif (-not $kAdmin) {
  Skip 'gui-settings-sysbt-buttons-real-toggle' 'radio state change requires elevation (unelevated no-harm click leg is gui-verify.ps1 section 3)'
} else {
  cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
  foreach ($i in 1..20) {   # pipe must be free or the GUI's PipeServer.start loses it
    Start-Sleep -Milliseconds 500
    if (-not (Test-PipeExists 'phantom-bt')) { break }
  }
  Start-Process -FilePath $gui -WorkingDirectory $root
  $k3mw = [IntPtr]::Zero
  foreach ($i in 1..20) {
    Start-Sleep -Milliseconds 500
    $k3p = Get-Process phantombt-gui -ErrorAction SilentlyContinue
    if ($k3p -and $k3p.MainWindowHandle -ne 0) { $k3mw = $k3p.MainWindowHandle; break }
  }
  $k3ok = $false; $k3why = 'not attempted'; $k3noRadio = $false
  if ($k3mw -eq [IntPtr]::Zero) {
    $k3why = 'GUI main window never appeared'
  } else {
    $k3up = $false
    foreach ($i in 1..20) {
      $k3t = Run-Cli @('cmd', 'status')
      if ($k3t.Rc -eq 0 -and $k3t.Out -match '^OK usbip-port=') { $k3up = $true; break }
      Start-Sleep -Milliseconds 500
    }
    if (-not $k3up) {
      $k3why = 'GUI bench never answered on the control pipe'
    } else {
      function Get-K3RadioRows {
        $rr = Run-Cli @('cmd', 'sys-bt list') 30
        return @($rr.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } | Where-Object { $_ -match '^\[(enabled|disabled)\]\s+RADIO' })
      }
      $k3rows = Get-K3RadioRows
      if ($k3rows.Count -eq 0) {
        $k3noRadio = $true   # CI-like machine: nothing to flip (F-section covers the no-radio reply)
      } else {
        $k3wasEnabled = @($k3rows | Where-Object { $_ -match '^\[enabled\]' }).Count -gt 0
        $k3n = $k3rows.Count
        $navSet = Find-ByCtrlId $k3mw 14      # nav "全局设置" (IDC_NAV_SETTINGS)
        $pageSet = Find-ByCtrlId $k3mw 5      # settings page CONTAINER id is 5 (mkpage(5))
        $btnOff = [IntPtr]::Zero; $btnOn = [IntPtr]::Zero
        if ($pageSet -ne [IntPtr]::Zero) { $btnOff = Find-ByCtrlId $pageSet 401; $btnOn = Find-ByCtrlId $pageSet 402 }
        $k3missNav = ($navSet -eq [IntPtr]::Zero)
        $k3missBtns = ($pageSet -ne [IntPtr]::Zero) -and (($btnOff -eq [IntPtr]::Zero) -or ($btnOn -eq [IntPtr]::Zero))
        if ($k3missNav -or $k3missBtns) {
          $k3why = ('controls missing: nav14=' + (-not $k3missNav) + ' page5=' + ($pageSet -ne [IntPtr]::Zero) + ' btn401=' + ($btnOff -ne [IntPtr]::Zero) + ' btn402=' + ($btnOn -ne [IntPtr]::Zero))
        } else {
          try {
            [G]::Click($navSet)
            Start-Sleep -Milliseconds 500
            if (-not [G]::IsWindowVisible($pageSet)) { throw 'settings page not shown after nav click' }
            [G]::Click($btnOff)   # -> run_and_show("sys-bt disable") on the GUI thread
            Start-Sleep -Milliseconds 800
            $k3log = Read-LogPage (Find-ByCtrlId $k3mw 3)
            $disN = @((Get-K3RadioRows) | Where-Object { $_ -match '^\[disabled\]' }).Count
            if ($k3log -notmatch '> sys-bt disable') { throw 'log page has no "> sys-bt disable" echo' }
            if ($disN -ne $k3n) { throw ("after disable: " + $disN + "/" + $k3n + " radios [disabled]") }
            [G]::Click($btnOn)    # -> run_and_show("sys-bt enable")
            Start-Sleep -Milliseconds 800
            $k3log2 = Read-LogPage (Find-ByCtrlId $k3mw 3)
            $enN = @((Get-K3RadioRows) | Where-Object { $_ -match '^\[enabled\]' }).Count
            if ($k3log2 -notmatch '> sys-bt enable') { throw 'log page has no "> sys-bt enable" echo' }
            if ($enN -ne $k3n) { throw ("after enable: " + $enN + "/" + $k3n + " radios [enabled]") }
            $k3ok = $true; $k3why = ''
          } catch {
            $k3why = $_.Exception.Message
          } finally {
            try {   # baseline restore no matter what happened above (same contract as H3)
              $nowRows = Get-K3RadioRows
              $nowEnabled = @($nowRows | Where-Object { $_ -match '^\[enabled\]' }).Count -gt 0
              if ($nowEnabled -ne $k3wasEnabled) {
                [void](Run-Cli @('cmd', ("sys-bt " + $(if ($k3wasEnabled) { 'enable' } else { 'disable' }))) 60)
              }
            } catch {}
          }
        }
      }
    }
  }
  if ($k3noRadio) {
    Skip 'gui-settings-sysbt-buttons-real-toggle' 'no physical bluetooth radio on this machine'
  } else {
    Check 'gui-settings-sysbt-buttons-real-toggle' $k3ok $k3why
  }
  cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
}

# ============ L. entry-row completion (round 3, serve flags + GUI 0-1 + wire) ============
# L1..L4 (rows: serve flag combos): legs rounds 1/2/3B never exercised on the exe
# -- --port pointing at an OCCUPIED port (Simulator retries want..want+9 and the
# banner must print the FALLBACK port, src/sim/simulator.cpp:47-67 + main.cpp:52),
# both --ble error legs (no --load-bt -> "ble <path>: ..." + exit 1, main.cpp:76-84;
# bad profile path likewise), and --load-bt as the ONLY flag verified through the
# pipe while the serve lives plus its self-exit code (round 1's all-flags run only
# checked the exit code of the combined invocation).
# L5..L8 (rows: GUI pages 0-1 buttons + embedded usbip server) run on a fresh GUI:
# wire-level OP_REQ_DEVLIST on the EMPTY bench, wire-level OP_REQ_IMPORT of the
# pipe-loaded 7-1 (round 2 only proved devlist-with-a-device on the wire), the
# detach button (104) WITH a listview selection (round 1 only proved the
# no-selection leg; the reply is the safe kernel no-op "not attached" -- real
# kernel imports stay on the real-device legs), and the type button (204)
# carrying the actual edit-203 text into the logged command echo.
# CONTENTION GUARD: the pipe name and the 3240 fallback window are shared by
# every phantom-bt-family bench on this machine (a uacsimd daemon holds
# 3240/3241 and exports 1d50:6222 -- it once made r2-gui-usbip-devlist-wire read
# a FOREIGN device). A freshly started GUI bench must answer devices=0; if the
# first status reply reports anything else the pipe is not ours and the two wire
# checks SKIP instead of false-failing (and nothing is loaded onto the foreign
# bench). The button checks L7/L8 are in-process (no pipe) and always run.

# ---- L1: --port busy -> banner reports the fallback port within want+1..want+9.
# The holder binds the SAME wildcard the usbip server binds (net.cpp:139 INADDR_ANY)
# -- a loopback-only holder does not collide with it on Windows.
$lbusy = Get-FreePort
$lhold = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, $lbusy)
$lhold.Start()
$r = Run-Cli @('serve', '--port', "$lbusy", '--exit-after-ms', '1200') 30
$lhold.Stop()
$lbPort = 0
if ($r.Err -match 'usbip server on port (\d+)') { $lbPort = [int]$matches[1] }
Check 'r3-serve-port-busy-falls-back' ($r.Rc -eq 0 -and $lbPort -gt $lbusy -and $lbPort -le ($lbusy + 9)) `
  ("rc=" + $r.Rc + "; wanted-port " + $lbusy + " held busy, banner port=" + $lbPort + " (fallback window " + ($lbusy + 1) + '..' + ($lbusy + 9) + ')')

# ---- L2: --ble error legs (ble-advertise depends on load-bt, simulator.cpp:147)
$r = Run-Cli @('serve', '--ble', 'profiles/ble_keyboard.json', '--exit-after-ms', '500') 30
Check 'r3-serve-ble-without-adapter-exit1' `
  ($r.Rc -eq 1 -and $r.Err -match [regex]::Escape('ble profiles/ble_keyboard.json')) `
  ("rc=" + $r.Rc + " err=" + ($r.Err -replace "`r`n", ' | ').Trim())

$r = Run-Cli @('serve', '--load-bt', '--ble', 'profiles/no-such-profile.json', '--exit-after-ms', '500') 30
Check 'r3-serve-ble-bad-profile-exit1' `
  ($r.Rc -eq 1 -and $r.Err -match [regex]::Escape('ble profiles/no-such-profile.json')) `
  ("rc=" + $r.Rc + " err=" + ($r.Err -replace "`r`n", ' | ').Trim())

# ---- L3: --load-bt alone -> device visible over the pipe while serve lives,
# and the serve then exits BY ITSELF with 0 at --exit-after-ms.
$lport3 = Get-FreePort
$serveL = Start-Process -FilePath $cli -ArgumentList 'serve', '--port', "$lport3", '--load-bt', '--exit-after-ms', '6000' `
  -WorkingDirectory $root -PassThru -WindowStyle Hidden
$lUp = $false
foreach ($i in 1..15) {
  Start-Sleep -Milliseconds 400
  if ($serveL.HasExited) { break }
  $lt = Run-Cli @('cmd', 'status')
  # exact-port guard: a foreign bench owning the pipe reports a different port
  if ($lt.Rc -eq 0 -and $lt.Out -match ('^OK usbip-port=' + $lport3 + ' ')) { $lUp = $true; break }
}
if (-not $lUp) {
  Check 'r3-serve-load-bt-visible-over-pipe' $false ('our serve on port ' + $lport3 + ' never answered on the pipe')
  if ($serveL -and -not $serveL.HasExited) { Stop-Process -Id $serveL.Id -Force -ErrorAction SilentlyContinue }
} else {
  $ll = Run-Cli @('cmd', 'list')
  $btOk = ($ll.Rc -eq 0 -and $ll.Out -match '(?m)^DEV 7-1 0b00:')
  $exited0 = $false
  try { Wait-Process -Id $serveL.Id -Timeout 15 -ErrorAction Stop; $exited0 = ($serveL.ExitCode -eq 0) } catch {}
  Check 'r3-serve-load-bt-visible-over-pipe' ($btOk -and $exited0) `
    ("list=" + (($ll.Out -replace "`r`n", ' | ').Trim()) + "; self-exit code 0=" + $exited0)
}

# ---- L4..L8: fresh GUI instance (in-process button checks + wire checks)
if ($SkipGui) {
  Skip 'r3-gui-devlist-empty-wire' '-SkipGui given (no interactive desktop in this environment)'
  Skip 'r3-gui-import-wire' '-SkipGui given'
  Skip 'r3-gui-detach-button-with-selection' '-SkipGui given'
  Skip 'r3-gui-type-button-carries-edit-text' '-SkipGui given'
} elseif (-not (Test-Path $gui)) {
  Skip 'r3-gui-devlist-empty-wire' ('phantombt-gui.exe not found at ' + $gui)
  Skip 'r3-gui-import-wire' 'gui exe missing'
  Skip 'r3-gui-detach-button-with-selection' 'gui exe missing'
  Skip 'r3-gui-type-button-carries-edit-text' 'gui exe missing'
} else {
  $r3sig = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections;
public class R3 {
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageSB(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageS(IntPtr h, uint msg, IntPtr wp, string lp);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static System.Collections.ArrayList Descendants(IntPtr root) {
    var l = new System.Collections.ArrayList();
    EnumChildWindows(root, (h, lp) => { l.Add(h); return true; }, IntPtr.Zero);
    return l;
  }
  public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
  public static IntPtr ChildById(IntPtr parent, int id) {
    foreach (var d in Descendants(parent)) { IntPtr h = (IntPtr)d; if (GetDlgCtrlID(h) == id) return h; }
    return IntPtr.Zero;
  }
  public static IntPtr ChildByClass(IntPtr parent, string cls) {
    foreach (var d in Descendants(parent)) { IntPtr h = (IntPtr)d; if (ClassOf(h) == cls) return h; }
    return IntPtr.Zero;
  }
  public static string GetText(IntPtr h) {
    IntPtr len = SendMessageW(h, 0x000E, IntPtr.Zero, IntPtr.Zero);
    var sb = new StringBuilder(len.ToInt32() + 2);
    SendMessageSB(h, 0x000D, (IntPtr)(len.ToInt32() + 2), sb);
    return sb.ToString();
  }
  public static void SetText(IntPtr h, string s) { SendMessageS(h, 0x000C, IntPtr.Zero, s); }
  public static void Click(IntPtr h) {
    RECT r; GetWindowRect(h, out r);
    int cx = (r.L + r.R) / 2 - r.L, cy = (r.T + r.B) / 2 - r.T;
    SendMessageW(h, 0x202, IntPtr.Zero, (IntPtr)(((cy & 0xFFFF) << 16) | (cx & 0xFFFF)));
  }
  // value-only LVM_SETSELECTIONMARK: pointer-to-struct messages do not marshal
  // cross-process (see the section I note; selected_busid reads this mark back).
  public static void SelectListItem(IntPtr lv, int idx) { SendMessageW(lv, 0x1043, IntPtr.Zero, (IntPtr)idx); }
}
'@
  Add-Type -TypeDefinition $r3sig
  function Read-R3Log([IntPtr]$mw) {
    $p3 = [R3]::ChildById($mw, 3)
    if ($p3 -eq [IntPtr]::Zero) { return '' }
    $e = [R3]::ChildByClass($p3, 'Edit')
    if ($e -eq [IntPtr]::Zero) { return '' }
    return [R3]::GetText($e)
  }

  cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
  foreach ($i in 1..20) {   # pipe must be free or the GUI's PipeServer.start loses it
    Start-Sleep -Milliseconds 500
    if (-not (Test-PipeExists 'phantom-bt')) { break }
  }
  Start-Process -FilePath $gui -WorkingDirectory $root
  $mwL = [IntPtr]::Zero
  foreach ($i in 1..20) {
    Start-Sleep -Milliseconds 500
    $gp = Get-Process phantombt-gui -ErrorAction SilentlyContinue
    if ($gp -and $gp.MainWindowHandle -ne 0) { $mwL = $gp.MainWindowHandle; break }
  }
  $lport = 0; $lFresh = $false
  if ($mwL -ne [IntPtr]::Zero) {
    foreach ($i in 1..20) {
      $t3 = Run-Cli @('cmd', 'status')
      if ($t3.Rc -eq 0 -and $t3.Out -match 'usbip-port=(\d+) devices=(\d+)') {
        $lport = [int]$matches[1]
        $lFresh = ($matches[2] -eq '0')
        break
      }
      Start-Sleep -Milliseconds 500
    }
  }

  if ($mwL -eq [IntPtr]::Zero) {
    Skip 'r3-gui-devlist-empty-wire' 'GUI main window never appeared'
    Skip 'r3-gui-import-wire' 'GUI main window never appeared'
  } elseif (-not $lFresh -or $lport -eq 0) {
    # contention: another phantom-bt-family bench owns the pipe (a fresh GUI bench
    # always answers devices=0). Never load anything onto the foreign bench.
    Skip 'r3-gui-devlist-empty-wire' ('pipe phantom-bt not ours: first status reply was usbip-port=' + $lport + ' (foreign bench owns it; wire checks need our own embedded server)')
    Skip 'r3-gui-import-wire' 'pipe phantom-bt not ours this run'
  } else {
    # --- L5: wire devlist on the EMPTY bench must report ndev=0
    $eOk = $false; $eWhy = 'devlist not attempted'
    try {
      $tc = New-Object System.Net.Sockets.TcpClient
      if ($tc.ConnectAsync('127.0.0.1', $lport).Wait(2000)) {
        $st = $tc.GetStream()
        $req = [byte[]](0x01, 0x11, 0x80, 0x05, 0, 0, 0, 0)   # 0x0111 + OP_REQ_DEVLIST + status 0
        $st.Write($req, 0, 8)
        $st.Flush()
        $hdr = Read-TcpExact $st 12
        $code = (([int]$hdr[2] -shl 8) -bor [int]$hdr[3])
        $ndev = (([int]$hdr[8] -shl 24) -bor ([int]$hdr[9] -shl 16) -bor ([int]$hdr[10] -shl 8) -bor [int]$hdr[11])
        if ($code -ne 0x0005) { $eWhy = ('reply code=0x{0:X4} (want 0x0005 OP_REP_DEVLIST)' -f $code) }
        elseif ($ndev -ne 0) {
          # diagnose WHO answered: a fresh GUI bench exports nothing, so a
          # non-empty list means the reported port is served by a foreign
          # phantom-bt-family bench (e.g. the resident uacsimd on 3240/3241,
          # device 1-1 1d50:6222) that raced us onto the pipe -- include the
          # identity in the failure line.
          $why = 'fresh bench reported ndev=' + $ndev + ' (want 0)'
          try {
            $blk = Read-TcpExact $st (312 * $ndev)
            $bid = ([System.Text.Encoding]::ASCII.GetString($blk, 256, 31)).Trim([char]0)
            $vv = (([int]$blk[300] -shl 8) -bor [int]$blk[301])
            $pp = (([int]$blk[302] -shl 8) -bor [int]$blk[303])
            $why += ('; foreign server? first dev busid=' + $bid + (' vid=0x{0:X4}' -f $vv) + (' pid=0x{0:X4}' -f $pp))
          } catch {}
          $eWhy = $why
        }
        else { $eOk = $true; $eWhy = ('port ' + $lport + ' replied 0x0111/0x0005 ndev=0') }
      } else { $eWhy = 'tcp connect to the embedded server timed out' }
      $tc.Close()
    } catch { $eWhy = 'exception: ' + $_.Exception.Message }
    Check 'r3-gui-devlist-empty-wire' $eOk $eWhy

    # --- L6: pipe-load 7-1, then wire-level OP_REQ_IMPORT of it (same handshake
    # a real usbip client performs; no kernel involvement on the server side).
    $iOk = $false; $iWhy = 'import not attempted'
    # NB: the CLI forwards ONLY argv[2] to the pipe (main.cpp:260) -- the whole
    # command must be one argument (same form as sections E and C).
    $lh = Run-Cli @('cmd', 'load-hid profiles/vendor.json')
    if (-not ($lh.Out -match '(?m)^OK 7-1')) {
      $iWhy = 'pipe load-hid reply: ' + $lh.Out.Trim()
    } else {
      try {
        $tc2 = New-Object System.Net.Sockets.TcpClient
        if ($tc2.ConnectAsync('127.0.0.1', $lport).Wait(2000)) {
          $st2 = $tc2.GetStream()
          $req2 = New-Object byte[] 40
          [byte[]]$pre2 = @(0x01, 0x11, 0x80, 0x03, 0, 0, 0, 0)   # 0x0111 + OP_REQ_IMPORT + status 0
          [Array]::Copy($pre2, 0, $req2, 0, 8)
          $bstr = [System.Text.Encoding]::ASCII.GetBytes('7-1')
          [Array]::Copy($bstr, 0, $req2, 8, $bstr.Length)         # busid[32], NUL padded
          $st2.Write($req2, 0, 40)
          $st2.Flush()
          $imp = Read-TcpExact $st2 8
          $icode = (([int]$imp[2] -shl 8) -bor [int]$imp[3])
          $istat = [int]$imp[7]   # last byte of the u32be status
          if ($istat -ne 0) {
            # refusal with the device loaded on the pipe bench means the wire
            # port belongs to a DIFFERENT server (contention) or 7-1 is already
            # imported by another connection -- include the pipe view for context
            $lx = Run-Cli @('cmd', 'list')
            $iWhy = ('import status=' + $istat + ' (busid 7-1 refused); pipe list=' + (($lx.Out -replace "`r`n", ' | ').Trim()))
          }
          elseif ($icode -ne 0x0003) { $iWhy = ('reply code=0x{0:X4} (want 0x0003 OP_REP_IMPORT)' -f $icode) }
          else {
            $idev = Read-TcpExact $st2 312
            $ibus = ([System.Text.Encoding]::ASCII.GetString($idev, 256, 31)).Trim([char]0)
            $ivid = (([int]$idev[300] -shl 8) -bor [int]$idev[301])
            $ipid = (([int]$idev[302] -shl 8) -bor [int]$idev[303])
            $iOk = (($ibus -eq '7-1') -and ($ivid -eq 0x1209) -and ($ipid -eq 0xB00D))
            $iWhy = ('busid=' + $ibus + ' vid=0x' + ('{0:X4}' -f $ivid) + ' pid=0x' + ('{0:X4}' -f $ipid))
          }
        } else { $iWhy = 'tcp connect to the embedded server timed out' }
        $tc2.Close()   # releases the wire import; the bench keeps running
      } catch { $iWhy = 'exception: ' + $_.Exception.Message }
    }
    Check 'r3-gui-import-wire' $iOk $iWhy
  }

  # --- L7: detach button (104) WITH a listview selection. Click 102 first:
  # load-bt is non-modal, loads a device on OUR sim (7-1 or 7-2) AND calls
  # refresh_device_list, so the listview is populated. Then select row 0 and
  # click 104: the command echo must carry the SELECTED busid (the no-selection
  # leg only logs "ERR ..." -- round 1 proved that), and the reply must be the
  # safe no-op ("OK not attached" when usbip.exe is installed, else the explicit
  # "ERR usbip.exe not found").
  # FINDING (2026-11-01 17:08 run): the 11:38 build-dir binaries predate the
  # detach-by-device fix (they embed "detach --all", 0 hits for "detach -p";
  # the fix is src/sim/simulator.cpp:431-437 in the working tree) -- there,
  # detach of a NON-imported busid still runs a kernel-wide `usbip detach --all`
  # and replies "OK detached". This check FAILS against such stale binaries BY
  # DESIGN and passes once the gate rebuilds from the current source.
  $dOk = $false; $dWhy = ''
  $pageDev = [IntPtr]::Zero
  if ($mwL -ne [IntPtr]::Zero) { $pageDev = [R3]::ChildById($mwL, 1) }
  $btn102 = [IntPtr]::Zero; $btn104 = [IntPtr]::Zero; $lvL = [IntPtr]::Zero
  if ($pageDev -ne [IntPtr]::Zero) {
    $btn102 = [R3]::ChildById($pageDev, 102)
    $btn104 = [R3]::ChildById($pageDev, 104)
    $lvL = [R3]::ChildByClass($pageDev, 'SysListView32')
  }
  if ($btn102 -eq [IntPtr]::Zero -or $btn104 -eq [IntPtr]::Zero -or $lvL -eq [IntPtr]::Zero) {
    $dWhy = 'device-page controls not found (page found=' + ($pageDev -ne [IntPtr]::Zero) + ')'
  } else {
    [R3]::Click($btn102)
    Start-Sleep -Milliseconds 800
    [R3]::SelectListItem($lvL, 0)
    [R3]::Click($btn104)
    Start-Sleep -Milliseconds 600
    $ltxt = Read-R3Log $mwL
    $echoOk = ($ltxt -match '> detach 7-\d')
    $replyOk = ($ltxt -match 'OK not attached') -or ($ltxt -match 'ERR usbip\.exe not found')
    $dOk = ($echoOk -and $replyOk)
    if (-not $dOk) { $dWhy = (('busidEcho=' + $echoOk + ' noOpReply=' + $replyOk + '; last log lines: ') + ((@($ltxt -split "`n") | Select-Object -Last 6) -join ' | ')) }
  }
  Check 'r3-gui-detach-button-with-selection' $dOk $dWhy

  # --- L8: type button (204) must carry the actual edit-203 text into the
  # command ("ble-type C0:11:22:33:44:55 <text>", src/gui/main.cpp:404-411).
  $tOk = $false; $tWhy = ''
  $pageBleL = [IntPtr]::Zero
  if ($mwL -ne [IntPtr]::Zero) { $pageBleL = [R3]::ChildById($mwL, 2) }
  $edit203 = [IntPtr]::Zero; $btn204 = [IntPtr]::Zero
  if ($pageBleL -ne [IntPtr]::Zero) {
    $edit203 = [R3]::ChildByClass($pageBleL, 'Edit')
    $btn204 = [R3]::ChildById($pageBleL, 204)
  }
  if ($edit203 -eq [IntPtr]::Zero -or $btn204 -eq [IntPtr]::Zero) {
    $tWhy = 'BLE-page type edit/button not found'
  } else {
    [R3]::SetText($edit203, 'r3token42')
    [R3]::Click($btn204)
    Start-Sleep -Milliseconds 600
    $lt2 = Read-R3Log $mwL
    $tOk = ($lt2 -match [regex]::Escape('> ble-type C0:11:22:33:44:55 r3token42'))
    if (-not $tOk) { $tWhy = 'log page missing the command echo with the edit-203 text' }
  }
  Check 'r3-gui-type-button-carries-edit-text' $tOk $tWhy

  # --- shutdown (same contract as sections E, I and K)
  if ($mwL -ne [IntPtr]::Zero) {
    [void][R3]::PostMessage($mwL, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
    foreach ($i in 1..20) {
      Start-Sleep -Milliseconds 500
      if ($null -eq (Get-Process phantombt-gui -ErrorAction SilentlyContinue)) { break }
    }
  }
  cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
}

# ==================== M. winrt-probe.exe process checks (round 3, probe rows) ====================
# Inventory rows "probe hid mode / probe radio mode (winrt radio + radio-registry
# fallback) / probe watcher mode / probe ble mode": the probe binary had ZERO
# automation (the run-tests chain never builds or runs it; run-m0..m3 only drive
# it inside the real-device segment). These are process-level checks against the
# real built probe binary -- the deterministic slices that need no usbip-win2
# driver, no admin rights and no real BLE peer (contracts from src/probes/main.cpp):
#   r3-probe-usage-contract       no args / unknown mode -> stderr usage + rc 3 (main.cpp:283-296)
#   r3-probe-hid-nodev-timeout    hid discovery of a vid:pid nothing on this machine
#                                 exposes -> rc 2 + {"result":"fail","reason":"nodev"}
#                                 after ~wait-ms (main.cpp:98-108)
#   r3-probe-radio-winrt-caliber  one-line JSON, rc in {0,2}, documented reason set
#                                 (name:*/state:N/noradio, main.cpp:148-163); when section
#                                 F's sys-bt list is available both radio calibers must
#                                 AGREE (enabled RADIO row -> rc 0; disabled -> state:N;
#                                 none -> noradio) -- this cross-check is exactly the M1
#                                 "registry vs WinRT Radios" dual-caliber concern
#   r3-probe-radio-registry-caliber  fallback caliber scans HKLM USB enum for
#                                 VID_0B00&PID_0001 (main.cpp:166-187): rc 0 +
#                                 reason driver-bound:... when the adapter was bound, or
#                                 rc 2 + nodev/no-usb-enum-key otherwise -- never a hang
#   r3-probe-watcher-graceful-fail unmatched --expect-name: rc 2 + fail JSON, reason
#                                 noname|noseen (main.cpp:189-229)
#   r3-probe-watcher-duration-honored  when the watcher loop actually ran, the process
#                                 must live >= --duration-ms
#   r3-probe-ble-arg-validation   --address not 12 hex chars -> rc 2 "bad address"
#                                 (main.cpp:235)
#   r3-probe-ble-connect-graceful-fail  an address that can never exist -> graceful
#                                 rc 2 fail JSON (nodev/gatt-status/...), never a hang
# The positive legs (real PhantomKb advert received, real GATT+HID node, radio On with
# our driver bound) remain real-device acceptance (run-m0..m3): not automatable without
# the hardware peer.
$r3probeExe = Join-Path $root 'build/release/src/winrt-probe.exe'
$r3probeNames = @('r3-probe-usage-contract', 'r3-probe-hid-nodev-timeout', 'r3-probe-radio-winrt-caliber',
                  'r3-probe-radio-registry-caliber', 'r3-probe-watcher-graceful-fail',
                  'r3-probe-watcher-duration-honored', 'r3-probe-ble-arg-validation',
                  'r3-probe-ble-connect-graceful-fail')
if (-not (Test-Path $r3probeExe)) {
  foreach ($n in $r3probeNames) { Skip $n ('winrt-probe.exe not found at ' + $r3probeExe) }
} else {
  # Probe runner: UTF-8 decode (the probe prints UTF-8), hard timeout, wall time.
  function Run-R3Probe([string[]]$pargs, [int]$timeoutMs = 30000) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $r3probeExe
    $psi.Arguments = ($pargs -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $oT = $p.StandardOutput.ReadToEndAsync()
    $eT = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($timeoutMs)) {
      try { $p.Kill() } catch {}
      $p.WaitForExit()
    }
    $p.WaitForExit()
    $sw.Stop()
    return @{ Rc = $p.ExitCode; Out = $oT.Result; Err = $eT.Result; Ms = $sw.ElapsedMilliseconds }
  }
  function ConvertTo-R3ProbeJson($run) {
    try { return ($run.Out.Trim() | ConvertFrom-Json) } catch { return $null }
  }

  # ---- M0: usage contract (no WinRT needed)
  $m0a = Run-R3Probe @() 15000
  $m0b = Run-R3Probe @('frobnicate') 15000
  Check 'r3-probe-usage-contract' `
    (($m0a.Rc -eq 3 -and $m0a.Err -match 'usage: winrt-probe') -and
     ($m0b.Rc -eq 3 -and $m0b.Err -match 'not implemented yet')) `
    ("noargs rc=" + $m0a.Rc + " err1='" + $m0a.Err.Trim() + "'; badmode rc=" + $m0b.Rc + " err2='" + $m0b.Err.Trim() + "'")

  # ---- M1: hid discovery-timeout path (0x1209:0xBEEF is no repo profile and no real
  # device on this machine; the loop must time out and report nodev, not hang).
  $m1 = Run-R3Probe @('hid', '--vid', '0x1209', '--pid', '0xBEEF', '--usage-page', '0xFF00',
                      '--usage', '0x01', '--wait-ms', '800') 30000
  $m1j = ConvertTo-R3ProbeJson $m1
  Check 'r3-probe-hid-nodev-timeout' `
    ($m1.Rc -eq 2 -and $m1j -and $m1j.result -eq 'fail' -and $m1j.reason -eq 'nodev' -and $m1.Ms -ge 700) `
    ("rc=" + $m1.Rc + " ms=" + $m1.Ms + " out='" + $m1.Out.Trim() + "'")

  # ---- M2: radio, WinRT caliber (+ dual-caliber agreement with sys-bt list)
  $m2 = Run-R3Probe @('radio') 30000
  $m2j = ConvertTo-R3ProbeJson $m2
  $m2ok = (($m2.Rc -eq 0 -or $m2.Rc -eq 2) -and $m2j -and
           (($m2j.result -eq 'pass') -or ($m2j.result -eq 'fail')) -and $m2j.reason)
  $m2detail = ("rc=" + $m2.Rc + " out='" + $m2.Out.Trim() + "'")
  # Cross-caliber against section F's sys-bt list (SetupAPI Bluetooth class). H3/K3
  # restore the radio baseline, so $sl (captured in F) still reflects current state.
  $m2cal = $null
  if ($sl -and $sl.Rc -eq 0) {
    $m2rows = @($sl.Out -split "`n" | ForEach-Object { $_.TrimEnd("`r") } |
      Where-Object { $_ -match '^\[(enabled|disabled)\]\s+RADIO' })
    if (@($m2rows | Where-Object { $_ -match '^\[enabled\]' }).Count -gt 0) { $m2cal = 'on' }
    elseif ($m2rows.Count -gt 0) { $m2cal = 'off' }
    else { $m2cal = 'none' }
  }
  if ($m2cal -eq 'on') {
    $m2ok = $m2ok -and ($m2.Rc -eq 0) -and ($m2j.result -eq 'pass') -and ($m2j.reason -match '^name:')
    $m2detail = 'sysbt: enabled RADIO row(s) present; ' + $m2detail
  } elseif ($m2cal -eq 'off') {
    $m2ok = $m2ok -and ($m2.Rc -eq 2) -and ($m2j.reason -match '^state:\d+')
    $m2detail = 'sysbt: RADIO row(s) all disabled; ' + $m2detail
  } elseif ($m2cal -eq 'none') {
    $m2ok = $m2ok -and ($m2.Rc -eq 2) -and ($m2j.reason -eq 'noradio')
    $m2detail = 'sysbt: no RADIO row; ' + $m2detail
  } else {
    $m2detail = 'sys-bt list unavailable this run, shape-only contract; ' + $m2detail
  }
  Check 'r3-probe-radio-winrt-caliber' $m2ok $m2detail

  # ---- M3: radio, registry-fallback caliber
  $m3 = Run-R3Probe @('radio-registry') 30000
  $m3j = ConvertTo-R3ProbeJson $m3
  Check 'r3-probe-radio-registry-caliber' `
    (($m3.Rc -eq 0 -and $m3j -and $m3j.result -eq 'pass' -and ($m3j.reason -match '^driver-bound:VID_0B00')) -or
     ($m3.Rc -eq 2 -and $m3j -and ($m3j.reason -eq 'nodev' -or $m3j.reason -eq 'no-usb-enum-key'))) `
    ("rc=" + $m3.Rc + " out='" + $m3.Out.Trim() + "'")

  # ---- M4: watcher, unmatched name -> graceful fail + duration honored
  $m4 = Run-R3Probe @('watcher', '--expect-name', 'PhantomR3NeverMatches', '--duration-ms', '1500') 30000
  $m4j = ConvertTo-R3ProbeJson $m4
  $m4ran = ($m4j -and ($m4j.reason -eq 'noname' -or $m4j.reason -eq 'noseen'))
  Check 'r3-probe-watcher-graceful-fail' `
    ($m4.Rc -eq 2 -and $m4j -and $m4j.result -eq 'fail' -and $m4j.reason) `
    ("rc=" + $m4.Rc + " ms=" + $m4.Ms + " out='" + $m4.Out.Trim() + "'")
  if ($m4ran) {
    Check 'r3-probe-watcher-duration-honored' ($m4.Ms -ge 1400) ("ms=" + $m4.Ms + " for --duration-ms 1500")
  } else {
    Skip 'r3-probe-watcher-duration-honored' ('watcher loop did not run this environment (reason=' + $(if ($m4j) { $m4j.reason } else { 'no-json' }) + ')')
  }

  # ---- M5: ble arg validation + graceful connect failure
  $m5 = Run-R3Probe @('ble', '--address', 'NOHEX') 15000
  $m5j = ConvertTo-R3ProbeJson $m5
  Check 'r3-probe-ble-arg-validation' ($m5.Rc -eq 2 -and $m5j -and $m5j.reason -eq 'bad address') `
    ("rc=" + $m5.Rc + " out='" + $m5.Out.Trim() + "'")
  $m6 = Run-R3Probe @('ble', '--address', '000000000000', '--wait-ms', '2000') 90000
  $m6j = ConvertTo-R3ProbeJson $m6
  Check 'r3-probe-ble-connect-graceful-fail' `
    ($m6.Rc -eq 2 -and $m6j -and $m6j.result -eq 'fail' -and $m6j.reason) `
    ("rc=" + $m6.Rc + " ms=" + $m6.Ms + " out='" + $m6.Out.Trim() + "'")
}

# ---------------------------------------------------------------- summary
[System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
$lines | ForEach-Object { Write-Output $_ }
Write-Output ('== ' + ($lines.Count - $script:fail - $script:skip) + ' passed, ' + $script:fail + ' failed, ' + $script:skip + ' skipped ==')
if ($script:fail -gt 0) { exit 1 }
exit 0
