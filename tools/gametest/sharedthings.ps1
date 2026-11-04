# Things a player set up that stayed on its side (Discord bug reports, 2026-11-03):
# 1. A display case (objDisplayCase) shows the same item at both players: the host puts an item in (its `socket`),
#    the guest's case takes it; the guest takes it out (noone), the host's case is empty again.
# 2. An arrow the host fires (objArrow made by the local player) shows at the guest as a copy, and the copy breaks
#    nothing there (no GML error, no arrow item dropped where it ends).
# Droids: droids.ps1.
# Walls/floors need the build menu and the placement cursor (objNewBuilding, UsePlaceableItem): see SCENARIOS.md.
# Run on a test slot. Prints PASS/FAIL per check. Usage: sharedthings.ps1 [-HostSlot 5]
param([int]$HostSlot = 5)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
. (Join-Path $PSScriptRoot 'lib.ps1')

$ObjDisplayCase = 145; $ObjArrow = 303
$SlotSocket = '0x7D0'
$NoOne = -4

$pair = StartPair -Slot $HostSlot
$h = $pair.Host; $g = $pair.Guest
"HOST=$h GUEST=$g"
Check 'the guest is in the host world' (WaitLog $g 'puppet: objPlayer' 90)
Start-Sleep 3

# The host's player: "id x y room".
$p = (Ask $h 'player') -split ' '
$px = [double]$p[1]; $py = [double]$p[2]

function FirstId([string]$list) { [regex]::Match($list, '^(\d+) ').Groups[1].Value }
function NoErrors { -not ((Get-Item "$env:LOCALAPPDATA\Forager\error.log" -ErrorAction SilentlyContinue).LastWriteTime -gt (Get-Process -Id $h).StartTime) }

# ---- 1. a display case ----
$cx = [math]::Floor($px / 16) * 16 + 8 + 32; $cy = [math]::Floor($py / 16) * 16 + 8
$case = Ask $h "call instance_create_depth $cx $cy -100 $ObjDisplayCase"
Start-Sleep 3  # the spawn reaches the guest; both sides see the new case empty once (displays.cpp scans every 0.5 s)
$guestCase = FirstId (Ask $g "list $ObjDisplayCase $cx $cy 4")
Check "the guest has the host's new display case ($guestCase)" ([bool]$guestCase)
$item = 3  # any item id
Ask $h "set $case $SlotSocket $item" | Out-Null
Start-Sleep 3
Check "the guest's case shows the host's item ($(Ask $g "var $guestCase $SlotSocket"))" ([double](Ask $g "var $guestCase $SlotSocket") -eq $item)
Ask $g "set $guestCase $SlotSocket $NoOne" | Out-Null
Start-Sleep 3
Check "the host's case is empty after the guest took the item ($(Ask $h "var $case $SlotSocket"))" ([double](Ask $h "var $case $SlotSocket") -eq $NoOne)
Check 'the guest still sees it empty (no back and forth)' ([double](Ask $g "var $guestCase $SlotSocket") -eq $NoOne)

# ---- 2. an arrow ----
$arrowsBefore = [int](Ask $g "count $ObjArrow")
Ask $h "call instance_create_depth $px $py -100 $ObjArrow" | Out-Null
Start-Sleep 1
$arrowsAfter = [int](Ask $g "count $ObjArrow")
Check "the host's arrow shows at the guest ($arrowsBefore -> $arrowsAfter)" ($arrowsAfter -gt $arrowsBefore)
Start-Sleep 6  # the copy flies on and ends like the shot (its collisions and Destroy_0 are skipped: shots.cpp)

Check 'no GML errors' (NoErrors)
Done
