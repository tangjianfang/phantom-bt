# tools/gui-verify.ps1 ? automated GUI verification over REAL control paths.
# Clicks are delivered as WM_LBUTTONUP directly to each control (the exact
# path a mouse click takes through the owner-draw procs); SendMessage(WM_
# COMMAND) is never used, so broken control->parent routing cannot hide.
# ASCII-only source. Exit code 0 = all checks passed.
$ErrorActionPreference = 'Stop'
$exe = 'C:/tjf/github/phantom-bt/build/release/src/phantombt-gui.exe'
$out = 'C:/tjf/github/phantom-bt/integration-results/gui-verify.txt'
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
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageSB(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);
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
    // Client coords relative to the control for the lParam.
    SendMessageW(h, 0x202, IntPtr.Zero, (IntPtr)(((cy - r.T) << 16) | ((cx - r.L) & 0xFFFF)));
  }
}
'@
Add-Type -TypeDefinition $sig

$lines = New-Object System.Collections.Generic.List[string]
$fail = 0
function Check([string]$name, [bool]$ok) {
  if ($ok) { $script:lines.Add('PASS ' + $name) }
  else { $script:lines.Add('FAIL ' + $name); $script:fail = $script:fail + 1 }
}

cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
Start-Sleep -Milliseconds 600
Start-Process $exe
Start-Sleep -Seconds 3
$mw = [G]::FindByTitlePart('phantom-bt')
Check 'main window appears' ($mw -ne [IntPtr]::Zero)
if ($mw -eq [IntPtr]::Zero) {
  [System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
  Write-Output ('FAILED: ' + $fail + ' checks'); exit 1
}

# --- control discovery (class + geometry based; no CJK title matching) ---
$navs = New-Object System.Collections.Generic.List[object]
$pages = New-Object System.Collections.Generic.List[object]
foreach ($k in [G]::Children($mw)) {
  $c = [G]::ClassOf($k)
  if ($c -eq 'PbNavItem') { $navs.Add($k) }
  if ($c -eq 'PbPage') { $pages.Add($k) }
}
Check 'five nav items exist' ($navs.Count -eq 5)
Check 'five pages exist' ($pages.Count -eq 5)

if ($navs.Count -eq 5 -and $pages.Count -eq 5) {

# --- 1) nav: global settings page (index 4) ---
[G]::Click($navs[4])
Start-Sleep -Milliseconds 700
Check 'nav click flips settings page' ([G]::IsWindowVisible($pages[4]))

# Settings-page controls: buttons by x-position (right-most pair per row),
# status STATICs by y-order.
$btns = New-Object System.Collections.Generic.List[object]
$stats = New-Object System.Collections.Generic.List[object]
foreach ($k in [G]::Children($pages[4])) {
  $c = [G]::ClassOf($k)
  if ($c -eq 'PbButton') { $btns.Add($k) }
  if ($c -eq 'Static') { $stats.Add($k) }
}
# Sort both by y then x (right-most first within a row).
$sortedBtns = @($btns | Sort-Object -Property @{Expression={ [G]::RectOf($_).T }}, @{Expression={ -[G]::RectOf($_).R }})
$sortedStats = @($stats | Sort-Object -Property @{Expression={ [G]::RectOf($_).T }})
Check 'settings page has 6 buttons' ($sortedBtns.Count -eq 6)
Check 'settings page has 8 statics (4 titles + 4 status)' ($sortedStats.Count -eq 8)
# Row layout (y ascending), right=primary first:
#   row1: btn[0]=disable, btn[1]=enable    row2: btn[2]=add, btn[3]=clear
#   row3: btn[4]=ble-add, btn[5]=ble-clear
# Statics interleaved by y: even index = section title, odd = status line
# (stats[1]=sys-bt, stats[3]=virtual-bt, stats[5]=ble).

if ($sortedBtns.Count -eq 6 -and $sortedStats.Count -eq 8) {
  # --- 2) virtual BT add / clear round-trip ---
  [G]::Click($sortedBtns[2])   # add virtual BT adapter
  Start-Sleep -Milliseconds 800
  $vbtText = [G]::GetText($sortedStats[3])
  Check 'vbt status shows 1 loaded' ($vbtText -match '1')
  [G]::Click($sortedBtns[3])   # clear (unload-all)
  Start-Sleep -Milliseconds 800
  $vbtText2 = [G]::GetText($sortedStats[3])
  Check 'vbt status back to none' ($vbtText2 -notmatch '[1-9]')

  # --- 3) permission path: sys-bt disable without elevation must not hang/crash ---
  [G]::Click($sortedBtns[0])   # disable physical radio (may ERR without admin)
  Start-Sleep -Milliseconds 1500
  $proc = Get-Process phantombt-gui -ErrorAction SilentlyContinue
  Check 'gui survives sys-bt disable click' ($null -ne $proc -and $proc.Responding)
  $sysbtText = [G]::GetText($sortedStats[1])
  Check 'sys-bt status line readable after op' ($sysbtText.Length -gt 0)

  # --- 4) BLE clear (no dialog path) ---
  [G]::Click($sortedBtns[5])   # ble-clear
  Start-Sleep -Milliseconds 700
  Check 'gui alive after ble-clear' ($null -ne (Get-Process phantombt-gui -ErrorAction SilentlyContinue))
}

# --- 5) auto-test page still functional end to end ---
[G]::Click($navs[3])
Start-Sleep -Milliseconds 500
Check 'nav click flips test page' ([G]::IsWindowVisible($pages[3]))
$runBtn = [IntPtr]::Zero
foreach ($k in [G]::Children($pages[3])) { if ([G]::ClassOf($k) -eq 'PbButton') { $runBtn = $k; break } }
Check 'run button found' ($runBtn -ne [IntPtr]::Zero)
if ($runBtn -ne [IntPtr]::Zero) {
  [G]::Click($runBtn)
  $deadline = (Get-Date).AddSeconds(120)
  $status = ''
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    foreach ($k in [G]::Children($pages[3])) {
      if ([G]::ClassOf($k) -eq 'Static') { $t = [G]::GetText($k); if ($t.Trim()) { $status += $t } }
    }
    if ($status -match '\d+/\d+') { break }
  }
  $green = $false
  if ($status -match '(\d+)\s*/\s*(\d+)') { $green = ($matches[1] -eq $matches[2]) }
  Check 'suite finished all-green' ($green)
  $lines.Add('  suite status: ' + $status)
}

}  # end if five navs/pages

cmd /c "taskkill /IM phantombt-gui.exe /F >nul 2>&1"
[System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
$lines | ForEach-Object { Write-Output $_ }
if ($fail -gt 0) { Write-Output ('FAILED: ' + $fail + ' checks failed'); exit 1 }
Write-Output 'ALL GUI CHECKS PASSED'
