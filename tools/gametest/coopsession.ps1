# Test helper: (re)start the host game if needed, load save slot 1 in both games, connect them.
# Usage: coopsession.ps1 -ClientPid 7320 [-HostPid 1234] -Port 7783 -Tag name
param([int]$ClientPid, [int]$HostPid = 0, [int]$Port, [string]$Tag)

$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
$logDir = Join-Path $env:TEMP "forager-coop-logs"; New-Item -ItemType Directory -Force $logDir | Out-Null
$exe = 'Z:\SteamLibrary\steamapps\common\Forager\Forager.exe'

function Cmd([int]$p, [string]$t) {
  & $ctl -GamePid $p -Key T | Out-Null; Start-Sleep -Milliseconds 300
  & $ctl -GamePid $p -Text $t | Out-Null; & $ctl -GamePid $p -Key Enter | Out-Null
  Start-Sleep -Milliseconds 500
}

if ($HostPid -eq 0 -or -not (Get-Process -Id $HostPid -ErrorAction SilentlyContinue)) {
  $proc = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -ArgumentList @('-debugoutput', "$logDir\host_$Tag.log") -PassThru
  $HostPid = $proc.Id
  Start-Sleep -Seconds 18
}
if (-not ('DialogKiller' -as [type])) {
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class DialogKiller {
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  // Presses Enter (the default "Yes") in every message box owned by the given process.
  public static int AcceptAll(uint target) {
    int n = 0;
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      var k = new StringBuilder(64); GetClassName(h, k, 64);
      if (pid == target && IsWindowVisible(h) && k.ToString() == "#32770") {
        PostMessage(h, 0x100, (IntPtr)0x0D, IntPtr.Zero); PostMessage(h, 0x101, (IntPtr)0x0D, IntPtr.Zero); n++;
      }
      return true; }, IntPtr.Zero);
    return n;
  }
}
'@
}
foreach ($p in $HostPid, $ClientPid) {
  & $ctl -GamePid $p -Click '255,95' -HoldMs 120 | Out-Null; Start-Sleep -Seconds 2
  & $ctl -GamePid $p -Click '1000,138' -HoldMs 120 | Out-Null
  Start-Sleep -Seconds 2
  # "This save contains content no longer present... Load anyway?" blocks loading
  $accepted = [DialogKiller]::AcceptAll([uint32]$p)
  if ($accepted -gt 0) { "accepted $accepted dialog(s) in $p" }
}
Start-Sleep -Seconds 12
Cmd $HostPid "/cohost $Port"
Start-Sleep -Seconds 1
Cmd $ClientPid "/cojoin 127.0.0.1 $Port"
Start-Sleep -Seconds 4
"HOST_PID=$HostPid"
