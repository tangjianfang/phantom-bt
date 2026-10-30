# Drive a live GUI's simulator via the control pipe: sys-bt list round-trip.
# ASCII-only source; UTF-8 output.
$out = 'C:/tjf/github/phantom-bt/integration-results/sysbt-cli.txt'
$exe = 'C:/tjf/github/phantom-bt/build/release/src/phantombt.exe'
$lines = New-Object System.Collections.Generic.List[string]
$p = Start-Process -FilePath $exe -ArgumentList 'serve','--load-bt','--exit-after-ms','8000' -RedirectStandardError "$env:TEMP\pb.err" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
# Control pipe protocol: send line, read until OK-END (same framing as CLI).
$sig = @'
using System;
using System.IO;
using System.IO.Pipes;
public class PipeCli {
  public static string[] Ask(string cmd) {
    using (var pipe = new NamedPipeClientStream(".", "phantom-bt", PipeDirection.InOut)) {
      pipe.Connect(3000);
      using (var w = new StreamWriter(pipe) { AutoFlush = true })
      using (var r = new StreamReader(pipe)) {
        w.WriteLine(cmd);
        var outLines = new System.Collections.Generic.List<string>();
        while (true) {
          string l = r.ReadLine();
          if (l == null) break;
          outLines.Add(l);
          if (l == "OK-END" || l.StartsWith("ERR")) break;
        }
        return outLines.ToArray();
      }
    }
  }
}
'@
Add-Type -TypeDefinition $sig
foreach ($cmd in @('sys-bt list','ble-clear','unload-all','status')) {
  $lines.Add('> ' + $cmd)
  try { foreach ($l in [PipeCli]::Ask($cmd)) { $lines.Add('  ' + $l) } }
  catch { $lines.Add('  EXC ' + $_.Exception.Message) }
}
Wait-Process -Id $p.Id -ErrorAction SilentlyContinue
[System.IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Output 'done'
