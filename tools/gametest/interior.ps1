# Interiors inside the world room (docs/coop/INTERIORS.md): the host goes into the druid tree; the world must go on
# for the guest meanwhile (the host's enemy updates keep coming); the door brings the host back out.
# Usage: interior.ps1 [-Room 23] [-Shot]
param([int]$Room = 23, [switch]$Shot, [int]$HostSlot = 4, [switch]$RealEntry)  # -RealEntry: in through objDruidTree (E), not room_goto
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
& (Join-Path $PSScriptRoot 'install.ps1')
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GodMode `
  -HostEnv @{COOP_DEBUG_PORT = '1'; COOP_DEBUG_ROOM = $(if ($RealEntry) { '' } else { "$Room" }); COOP_DEBUG_ROOM_AT = '45'} `
  -GuestEnv @{COOP_DEBUG_PORT = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
$hostLog = "$GameDir\coop_native_$h.log"; $guestLog = "$GameDir\coop_native_$g.log"
"HOST=$h GUEST=$g"
if ($RealEntry) {
  Start-Sleep 3
  $tree = [regex]::Match((Ask $h 'woken 165 list 165'), '^\d+: (\d+)').Groups[1].Value  # objDruidTree
  "druid tree ${tree}: $(Ask $h "woken 165 event $tree 7 11")"
}
for ($i = 0; $i -lt 90 -and -not (Select-String -Path $hostLog -Pattern 'interiors: in room' -Quiet); $i++) { Start-Sleep 1 }
Check 'all hooks installed (session ok)' (Select-String -Path $hostLog -Pattern 'hooks enabled \(session ok' -Quiet)
Check 'the host went in without leaving the world room' (Select-String -Path $hostLog -Pattern "interiors: in room $Room inside the world room" -Quiet)
"host: $(Ask $h 'player')"
$updatesBefore = @(Select-String -Path $guestLog -Pattern 'mobs: update #').Count
$reconcileBefore = @(Select-String -Path $hostLog -Pattern 'sync: sent spawn').Count
Start-Sleep 2
if ($Shot) { & $ctl -GamePid $h -Shot 'interior_in' | Out-Null; & $ctl -GamePid $g -Shot 'interior_guest' | Out-Null }
Start-Sleep 10
$updates = @(Select-String -Path $guestLog -Pattern 'mobs: update #').Count - $updatesBefore
$stats = @(Select-String -Path $hostLog -Pattern 'sync: sent spawn').Count - $reconcileBefore
$puppet = [regex]::Matches((Ask $g 'list 22'), '(\d+) ([\d.]+) ([\d.]+);') | Where-Object { [double]$_.Groups[2].Value -gt 1700 } | Select-Object -First 1
Check "the host's world goes on while it is inside ($stats sync-stats lines in 12 s) and reaches the guest (host's puppet inside: $($puppet.Value))" ($stats -gt 0 -and $puppet)
$door = [regex]::Match((Ask $h 'list 276'), '^\d+: (\d+)').Groups[1].Value
Check "the interior has its door ($door)" ([bool]$door)
if ($door) { Ask $h "event $door 7 11" | Out-Null }
for ($i = 0; $i -lt 10 -and -not (Select-String -Path $hostLog -Pattern 'interiors: left room' -Quiet); $i++) { Start-Sleep 1 }
Start-Sleep 2
Check "the door brought the host back out ($(Ask $h 'player'))" (Select-String -Path $hostLog -Pattern 'interiors: left room' -Quiet)
if ($Shot) { & $ctl -GamePid $h -Shot 'interior_out' | Out-Null }
Check 'the host never left the world room' (-not (Select-String -Path $hostLog -Pattern 'session: left the world room' -Quiet))
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
