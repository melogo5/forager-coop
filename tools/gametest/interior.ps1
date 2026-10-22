# Interiors inside the world room (docs/coop/INTERIORS.md): a player goes into an interior (the host, or the guest with
# -OnGuest); the world must go on meanwhile (the host's sync keeps running, the other side sees the player's puppet
# inside); the door brings the player back out, and nobody leaves the world room.
# Usage: interior.ps1 [-Room 23] [-OnGuest] [-RealEntry] [-Shot]
param([int]$Room = 23, [switch]$Shot, [int]$HostSlot = 4, [switch]$RealEntry, [switch]$OnGuest)  # -RealEntry: in through objDruidTree (E), not room_goto
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
& (Join-Path $PSScriptRoot 'install.ps1')
$visit = @{COOP_DEBUG_ROOM = $(if ($RealEntry) { '' } else { "$Room" }); COOP_DEBUG_ROOM_AT = '10'}
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GodMode `
  -HostEnv (@{COOP_DEBUG_PORT = '1'} + $(if ($OnGuest) { @{} } else { $visit })) `
  -GuestEnv (@{COOP_DEBUG_PORT = '1'} + $(if ($OnGuest) { $visit } else { @{} }))
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
$hostLog = "$GameDir\coop_native_$h.log"
# The player who goes in, and the other one.
$in = $(if ($OnGuest) { $g } else { $h }); $out = $(if ($OnGuest) { $h } else { $g })
$inLog = "$GameDir\coop_native_$in.log"
$who = $(if ($OnGuest) { 'guest' } else { 'host' })
"HOST=$h GUEST=$g; the $who goes in"
if ($RealEntry) {
  Start-Sleep 3
  $tree = [regex]::Match((Ask $in 'woken 165 list 165'), '^\d+: (\d+)').Groups[1].Value  # objDruidTree
  "druid tree ${tree}: $(Ask $in "woken 165 event $tree 7 11")"
}
for ($i = 0; $i -lt 90 -and -not (Select-String -Path $inLog -Pattern 'interiors: in room' -Quiet); $i++) { Start-Sleep 1 }
Check 'all hooks installed (session ok)' (Select-String -Path $inLog -Pattern 'hooks enabled \(session ok' -Quiet)
Check "the $who went in without leaving the world room" (Select-String -Path $inLog -Pattern "interiors: in room $Room inside the world room" -Quiet)
"${who}: $(Ask $in 'player')"
$statsBefore = @(Select-String -Path $hostLog -Pattern 'sync: sent spawn').Count
Start-Sleep 2
if ($Shot) { & $ctl -GamePid $in -Shot 'interior_in' | Out-Null; & $ctl -GamePid $out -Shot 'interior_out_side' | Out-Null }
Start-Sleep 10
$stats = @(Select-String -Path $hostLog -Pattern 'sync: sent spawn').Count - $statsBefore
$puppet = [regex]::Matches((Ask $out 'list 22'), '(\d+) ([\d.]+) ([\d.]+);') | Where-Object { [double]$_.Groups[2].Value -gt 1700 } | Select-Object -First 1
Check "the host's world goes on ($stats sync-stats lines in 12 s); the other side sees the $who inside ($($puppet.Value))" ($stats -gt 0 -and $puppet)
$door = [regex]::Match((Ask $in 'list 276'), '^\d+: (\d+)').Groups[1].Value
Check "the interior has its door ($door)" ([bool]$door)
if ($door) { Ask $in "event $door 7 11" | Out-Null }
for ($i = 0; $i -lt 10 -and -not (Select-String -Path $inLog -Pattern 'interiors: left room' -Quiet); $i++) { Start-Sleep 1 }
Start-Sleep 2
Check "the door brought the $who back out ($(Ask $in 'player'))" (Select-String -Path $inLog -Pattern 'interiors: left room' -Quiet)
if ($Shot) { & $ctl -GamePid $in -Shot 'interior_out' | Out-Null }
Check "the $who never left the world room" (-not (Select-String -Path $inLog -Pattern 'session: left the world room' -Quiet))
Check 'no GML errors' (-not ((Get-Item "$env:LOCALAPPDATA\Forager\error.log" -ErrorAction SilentlyContinue).LastWriteTime -gt (Get-Process -Id $h).StartTime))
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
