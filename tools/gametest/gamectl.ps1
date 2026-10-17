# Background control of a game window via window messages (no real mouse/keyboard).
# Usage:
#   gamectl.ps1 -GamePid 1234 -Shot name          screenshot of the client area -> shots\name.png
#   gamectl.ps1 -GamePid 1234 -Key T              key press (WM_KEYDOWN/UP); names: Enter, Esc, Back, Tab, F1.., letters, digits
#   gamectl.ps1 -GamePid 1234 -Text "/costatus"   types text (WM_CHAR per character)
#   gamectl.ps1 -GamePid 1234 -Click 640,360      left click at client coords (WM_MOUSEMOVE + LBUTTON)
#   gamectl.ps1 -List                             list Forager windows
param(
  [int]$GamePid = 0,
  [string]$Shot = '',
  [string]$Key = '',
  [string]$Text = '',
  [string]$Click = '',
  [switch]$List,
  [int]$HoldMs = 60
)

$ShotDir = Join-Path $PSScriptRoot 'shots'
New-Item -ItemType Directory -Force $ShotDir | Out-Null
Add-Type -AssemblyName System.Drawing
if (-not ('GameCtl' -as [type])) {
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class GameCtl {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
}
'@
}

$WM_KEYDOWN = 0x100; $WM_KEYUP = 0x101; $WM_CHAR = 0x102
$WM_MOUSEMOVE = 0x200; $WM_LBUTTONDOWN = 0x201; $WM_LBUTTONUP = 0x202

function Get-Window([int]$id) {
  $p = Get-Process -Id $id -ErrorAction Stop
  if ($p.MainWindowHandle -eq 0) { throw "process $id has no window" }
  return $p.MainWindowHandle
}

function Get-VirtualKey([string]$name) {
  switch ($name.ToUpper()) {
    'ENTER' { return 0x0D } 'ESC' { return 0x1B } 'BACK' { return 0x08 } 'TAB' { return 0x09 }
    'SPACE' { return 0x20 } 'UP' { return 0x26 } 'DOWN' { return 0x28 } 'LEFT' { return 0x25 } 'RIGHT' { return 0x27 }
    default {
      if ($name -match '^F(\d{1,2})$') { return 0x6F + [int]$Matches[1] }
      if ($name.Length -eq 1) { return [int][char]$name.ToUpper() }
      throw "unknown key '$name'"
    }
  }
}

function Send-Key([IntPtr]$h, [string]$name) {
  $vk = Get-VirtualKey $name
  $scan = [int64][GameCtl]::MapVirtualKey($vk, 0)
  $down = [int64]1 -bor ($scan -shl 16)
  $up = $down -bor ([int64]1 -shl 30) -bor ([int64]1 -shl 31)
  [GameCtl]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$vk, [IntPtr]$down) | Out-Null
  Start-Sleep -Milliseconds $HoldMs
  [GameCtl]::PostMessage($h, $WM_KEYUP, [IntPtr]$vk, [IntPtr]$up) | Out-Null
}

function Send-Text([IntPtr]$h, [string]$s) {
  foreach ($c in $s.ToCharArray()) {
    [GameCtl]::PostMessage($h, $WM_CHAR, [IntPtr][int]$c, [IntPtr]1) | Out-Null
    Start-Sleep -Milliseconds 15
  }
}

function Send-Click([IntPtr]$h, [int]$x, [int]$y) {
  $lp = [IntPtr]([int64](($y -shl 16) -bor ($x -band 0xFFFF)))
  [GameCtl]::PostMessage($h, $WM_MOUSEMOVE, [IntPtr]0, $lp) | Out-Null
  # menu buttons react only after the cursor has hovered them for a few frames
  Start-Sleep -Milliseconds 300
  [GameCtl]::PostMessage($h, $WM_MOUSEMOVE, [IntPtr]0, $lp) | Out-Null
  Start-Sleep -Milliseconds 100
  [GameCtl]::PostMessage($h, $WM_LBUTTONDOWN, [IntPtr]1, $lp) | Out-Null
  Start-Sleep -Milliseconds $HoldMs
  [GameCtl]::PostMessage($h, $WM_LBUTTONUP, [IntPtr]0, $lp) | Out-Null
}

function Save-Shot([IntPtr]$h, [string]$name) {
  $r = New-Object GameCtl+RECT
  [GameCtl]::GetClientRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R)), ([Math]::Max(1, $r.B))
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [GameCtl]::PrintWindow($h, $hdc, 3) | Out-Null
  $g.ReleaseHdc($hdc); $g.Dispose()
  $file = Join-Path $ShotDir "$name.png"
  $bmp.Save($file); $bmp.Dispose()
  return $file
}

if ($List) {
  Get-Process Forager -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } |
    Select-Object Id, StartTime, MainWindowTitle | Format-Table -AutoSize
  return
}

$h = Get-Window $GamePid
if ($Key) { Send-Key $h $Key }
if ($Text) { Send-Text $h $Text }
if ($Click) { $xy = $Click.Split(','); Send-Click $h ([int]$xy[0]) ([int]$xy[1]) }
if ($Shot) { Start-Sleep -Milliseconds 250; Save-Shot $h $Shot }
