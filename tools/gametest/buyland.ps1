# PLAYTEST3 #3: on an island the host buys, the treasure chests stand the same at the guest (the guest saw one
# "crooked"). The host buys the island of the nearest sale sign (COOP_DEBUG_BUYLAND); the guest is asked about every
# chest the host has: same spot. Usage: buyland.ps1 [-HostSlot 4] [-At 25]
param([int]$HostSlot = 4, [int]$At = 25)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
& (Join-Path $PSScriptRoot 'install.ps1')
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GodMode `
  -HostEnv @{COOP_DEBUG_PORT = '1'; COOP_DEBUG_BUYLAND = "$At"} -GuestEnv @{COOP_DEBUG_PORT = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
$hostLog = "$GameDir\coop_native_$h.log"
# Sale signs exist only while the BUY LAND tab of the player menu is open.
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
& $ctl -GamePid $h -Key Esc | Out-Null; Start-Sleep 1
& $ctl -GamePid $h -Click '735,60' -HoldMs 300 | Out-Null; Start-Sleep -Milliseconds 400; & $ctl -GamePid $h -Click '735,60' -HoldMs 150 | Out-Null
for ($i = 0; $i -lt 60 -and -not (Select-String -Path $hostLog -Pattern 'land: debug bought|debug purchase: no' -Quiet); $i++) { Start-Sleep 1 }
& $ctl -GamePid $h -Key Esc | Out-Null
Start-Sleep 5
$bought = Select-String -Path $hostLog -Pattern 'land: debug bought the island at \((\d+), (\d+)\)' | Select-Object -First 1
"HOST=$h GUEST=$g; $(if ($bought) { $bought.Line } else { 'nothing bought' })"
Check 'the host bought an island' ([bool]$bought)
# PLAYTEST8 #1: islands bought with a guest in the game came out a little different there. Land vs water on a
# 16 px grid over the whole world, at both.
$hostMap = (Ask $h 'watermap 8 8 1336 1336 16').Split('/'); $guestMap = (Ask $g 'watermap 8 8 1336 1336 16').Split('/')
$landDiff = @()
for ($r = 0; $r -lt $hostMap.Count; $r++) {
  for ($c = 0; $c -lt $hostMap[$r].Length; $c++) {
    if ($hostMap[$r][$c] -ne $guestMap[$r][$c]) { $landDiff += "($(8 + 16 * $c), $(8 + 16 * $r)) host $($hostMap[$r][$c])" }
  }
}
Check "land and water are the same at the guest ($($landDiff.Count) points differ$(if ($landDiff) { ': ' + (($landDiff | Select-Object -First 12) -join '; ') }))" ($landDiff.Count -eq 0)
# Every chest (all four kinds) of a game, woken up: "object at (x, y)".
function Chests([int]$game) {
  foreach ($object in 150, 151, 152, 153) {
    foreach ($m in [regex]::Matches((Ask $game "woken $object list $object"), '(\d+) ([\d.-]+) ([\d.-]+);')) {
      "$object at ($($m.Groups[2].Value), $($m.Groups[3].Value))"
    }
  }
}
$hostChests = @(Chests $h); $guestChests = @(Chests $g)
"host chests: $($hostChests -join '; ')"
"guest chests: $($guestChests -join '; ')"
if ($hostChests.Count -eq 0) { 'note: no chest at the host at all (chests on new islands are random) - nothing to compare'; Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue; return }
$missing = @($hostChests | Where-Object { $guestChests -notcontains $_ })
$extra = @($guestChests | Where-Object { $hostChests -notcontains $_ })
Check "every chest of the host stands on the same spot at the guest$(if ($missing) { ' - not: ' + ($missing -join ', ') })" ($missing.Count -eq 0)
Check "the guest has no chests the host has not$(if ($extra) { ' - extra: ' + ($extra -join ', ') })" ($extra.Count -eq 0)
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
