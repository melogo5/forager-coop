# PLAYTEST3 #5a: a craft the host started before the guest joined comes with the host's world to the guest. It is
# the host's: the guest's copy of the station must not make it (the guest claimed it and got the shovel before).
# Needs a save with a furnace by the start (e.g. a copy of a played world). Usage: savedcraft.ps1 [-HostSlot 4]
param([int]$HostSlot = 4)  # the save slot: a world with structures by the start (saveData<n>)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
& (Join-Path $PSScriptRoot 'install.ps1')
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GodMode `
  -HostEnv @{COOP_DEBUG_CRAFT = '1'; COOP_DEBUG_CRAFT_COUNT = '40'; COOP_DEBUG_STATION_OBJECT = '109'; COOP_DEBUG_PORT = '1'} -GuestEnv @{COOP_DEBUG_CRAFTWATCH = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
Start-Sleep 40
function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }
$hostLog = Get-Content "$d\coop_native_$h.log"; $guestLog = Get-Content "$d\coop_native_$g.log"
"HOST=$h GUEST=$g"
& (Join-Path $PSScriptRoot 'query.ps1') -GamePid $h 'player' 'list 108' 'list 109' 
$started = $hostLog | Select-String 'debug started crafting' | Select-Object -First 1
Check "the host started a craft before the guest joined ($started)" ([bool]$started)
$theirs = $guestLog | Select-String "(came with the host's world|showing the other player's craft) at station 109" | Select-Object -First 1
Check "the guest shows it as the host's ($theirs)" ([bool]$theirs)
Check 'the guest got nothing from it' (-not ($guestLog | Select-String 'crafted items straight into the inventory' -Quiet))
Check 'the host did not hand its station over (no mirror of its own craft)' (-not ($hostLog | Select-String "showing the other player's craft" -Quiet))
$last = $hostLog | Select-String 'stations: debug: station \d+ crafting \[1, (\d+)\]' | Select-Object -Last 1
Check "the host's own craft goes on ($($last.Line))" ($last -and [int]$last.Matches[0].Groups[1].Value -lt 40)
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
