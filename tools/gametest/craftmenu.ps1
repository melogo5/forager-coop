# Playtest 11: a guest's own craft, started from the station's menu, is the guest's - and stays the guest's while
# the station sleeps (the guest walked off-screen).
# 1. uiCrafting's Other_13 sets `crafting` and destroys the menu in the same event; the guest took its own craft
#    for one that came with the host's world, and its furnace never made anything. Mutant twin: craftmenu.
# 2. A station asleep at the guest dropped out of the guest's scan: "left" went to the host, which freed its copy
#    (and could start its own craft there, overwriting the guest's). Mutant twin: craftasleep.
# Needs a save with a furnace by the start, as savedcraft.ps1. Usage: craftmenu.ps1 [-HostSlot 4] [-Mutant craftmenu]
param([int]$HostSlot = 4, [string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
Set-Location (Split-Path (Split-Path $PSScriptRoot))
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
& (Join-Path $PSScriptRoot 'install.ps1')
$both = @{COOP_DEBUG_PORT = '1'}
if ($Mutant) { $both.COOP_DEBUG_MUTANT = $Mutant }
$guestEnv = $both.Clone()
$guestEnv += @{COOP_DEBUG_STATION = '6'; COOP_DEBUG_STATION_OBJECT = '109'; COOP_DEBUG_CRAFT = '1'
  COOP_DEBUG_CRAFT_COUNT = '40'; COOP_DEBUG_CRAFT_MENU = '1'; COOP_DEBUG_CRAFTWATCH = '1'}
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GodMode -HostEnv $both -GuestEnv $guestEnv
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
"HOST=$h GUEST=$g mutant=$Mutant"
function GuestLog { Get-Content "$GameDir\coop_native_$g.log" }
function HostLog { Get-Content "$GameDir\coop_native_$h.log" }
function LeftCount { @(HostLog | Select-String 'the other player left station 109').Count }
$theHosts = "came with the host's world"
Start-Sleep 25

$started = GuestLog | Select-String 'debug started crafting item \d+ at (\d+) \(menu closed\)' | Select-Object -First 1
Check "the guest started a craft from the menu, which closed ($($started.Line))" ([bool]$started)
Check 'the guest keeps it as its own (not "came with the host''s world")' (-not [bool](GuestLog | Select-String $theHosts -Quiet))
Check 'the host shows the guest''s craft' ([bool](HostLog | Select-String "showing the other player's craft at station 109" -Quiet))
Check 'the guest got crafted items' ([bool](GuestLog | Select-String 'crafted items straight into the inventory' -Quiet))
if (-not $started) { Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue; Done }

# The guest walks away: the game's culling puts the station to sleep at the guest.
$station = [int]$started.Matches[0].Groups[1].Value
$leftBefore = LeftCount
$me = (Ask $g 'player') -split ' '  # id x y room
Ask $g "moveto $($me[0]) $([double]$me[1] + 1500) $($me[2])" | Out-Null
Start-Sleep 6
$asleep = Ask $g "obj $station"
Check "the station sleeps at the guest ($asleep)" ($asleep -match 'asleep')
$leftAsleep = LeftCount
Check "while asleep at the guest, the host keeps the station in use ('left' $leftBefore -> $leftAsleep)" ($leftAsleep -eq $leftBefore)
Ask $g "moveto $($me[0]) $($me[1]) $($me[2])" | Out-Null
Start-Sleep 4
Check 'back again, the craft is still the guest''s own' (-not [bool](GuestLog | Select-String $theHosts -Quiet))
Check "and the host did not free the station ('left' $leftBefore -> $(LeftCount))" ((LeftCount) -eq $leftBefore)
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Done
