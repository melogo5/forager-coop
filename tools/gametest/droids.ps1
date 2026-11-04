# Droids are seen by every player (PLAYTEST10.md #11, familiars.cpp): each player gets a droid (GainItem, as crafting
# gives one) and both players must have it on screen - drawn (its Draw_0 ran, not skipped for a missing look), inside
# the camera's view, next to its owner (the player itself, or its puppet at the other side). Then each side's room
# starts again (the game makes every droid again from global.droids, copies are gone): the copies must come back and
# the guest's own droids must stay its own. Screenshots: shots\droids_*.png.
# Run on a test slot. Prints PASS/FAIL per check. Usage: droids.ps1 [-HostSlot 5] [-NoInstall]
param([int]$HostSlot = 5, [switch]$NoInstall)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
. (Join-Path $PSScriptRoot 'lib.ps1')
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'

$ScriptGainItem = '6ac690'  # decompiled/code/index.tsv
$ItemDroid = 113            # the droid item (PLAYTEST9.md #4)
$NearOwner = 96             # pixels: a droid flies around its owner
$CopyWait = 4               # seconds: a familiars check each second, the message, the copy at the next check

if (-not $NoInstall) { & (Join-Path $PSScriptRoot 'install.ps1') }
$pair = StartPair -Slot $HostSlot
$h = $pair.Host; $g = $pair.Guest
"HOST=$h GUEST=$g"
Check 'the guest is in the host world' (WaitLog $g 'puppet: objPlayer' 90)
Start-Sleep 3
# An older co-op loaded besides version.dll (a Workshop package's catch_error.dll in %LOCALAPPDATA%\Forager) answers
# the debug port without `droids`.
if ((Ask $h 'droids') -like 'unknown command*') {
  Check 'the debug port knows `droids` (is an older co-op from %LOCALAPPDATA%\Forager loaded too?)' $false
  Done
}

# The debug port's `droids`: "id x y drawn D screen S whom W at N; ..." -> objects.
function Droids([int]$game) {
  $answer = Ask $game 'droids'
  if ($answer -eq 'none') { return @() }
  foreach ($m in [regex]::Matches($answer, '(\d+) (-?\d+) (-?\d+) drawn (\d) screen (-?\d) whom (\w+) at (-?\d+)')) {
    [pscustomobject]@{
      id = [int]$m.Groups[1].Value; drawn = $m.Groups[4].Value -eq '1'; screen = $m.Groups[5].Value -eq '1'
      whom = $m.Groups[6].Value; at = [int]$m.Groups[7].Value
    }
  }
}
function Shown($droid) { $droid.drawn -and $droid.screen -and $droid.at -ge 0 -and $droid.at -le $NearOwner }
function Describe($droids) {
  if (@($droids).Count -eq 0) { return 'none' }
  (@($droids) | ForEach-Object { "$($_.id) $($_.whom) drawn=$($_.drawn) screen=$($_.screen) at=$($_.at)" }) -join '; '
}
# How many droids of `whom` (self / puppet) are on screen next to it; how many are not shown at all.
function CountShown($droids, [string]$whom) { @(@($droids) | Where-Object { $_.whom -eq $whom -and (Shown $_) }).Count }
function CountHidden($droids) { @(@($droids) | Where-Object { -not (Shown $_) }).Count }
# The size of a game's global.droids (0x1BA): the droids its world makes at a room start.
function WorldDroids([int]$game) { [int](Ask $game "call ds_list_size $(Ask $game 'global 0x1BA')") }

# One look at both sides: the four counts and the hidden ones.
function Look {
  $hostNow = @(Droids $h); $guestNow = @(Droids $g)
  [pscustomobject]@{
    hostSelf = CountShown $hostNow 'self'; hostPuppet = CountShown $hostNow 'puppet'
    guestSelf = CountShown $guestNow 'self'; guestPuppet = CountShown $guestNow 'puppet'
    hidden = (CountHidden $hostNow) + (CountHidden $guestNow)
    text = "host: $(Describe $hostNow) | guest: $(Describe $guestNow)"
  }
}
# The checks of one stage: what each side should show, and nothing hidden.
function Expect([string]$stage, $now, [int]$hostSelf, [int]$hostPuppet, [int]$guestSelf, [int]$guestPuppet) {
  "  $stage - $($now.text)"
  Check "${stage}: the host has its own $hostSelf droid(s) on screen next to it ($($now.hostSelf))" ($now.hostSelf -eq $hostSelf)
  Check "${stage}: the host shows the guest's $hostPuppet droid(s) next to the guest's puppet ($($now.hostPuppet))" ($now.hostPuppet -eq $hostPuppet)
  Check "${stage}: the guest has its own $guestSelf droid(s) on screen next to it ($($now.guestSelf))" ($now.guestSelf -eq $guestSelf)
  Check "${stage}: the guest shows the host's $guestPuppet droid(s) next to the host's puppet ($($now.guestPuppet))" ($now.guestPuppet -eq $guestPuppet)
  Check "${stage}: no droid hidden, off screen or away from its owner ($($now.hidden))" ($now.hidden -eq 0)
}

# Both players side by side: each one's camera has the other in view.
$hp = (Ask $h 'player') -split ' '
$gp = (Ask $g 'player') -split ' '
Ask $g "moveto $($gp[0]) $([double]$hp[1] + 32) $([double]$hp[2])" | Out-Null
Start-Sleep 3

# The world and the guest's character may have droids already (a guest's come back from the last game).
$start = Look
"at the start: $($start.text)"
Check "at the start: the guest's droids are seen by the host, the host's by the guest" (
  $start.hostPuppet -eq $start.guestSelf -and $start.guestPuppet -eq $start.hostSelf -and $start.hidden -eq 0)

# ---- 1. the host gets a droid ----
Ask $h "script $ScriptGainItem $ItemDroid" | Out-Null
Start-Sleep $CopyWait
Expect 'the host got a droid' (Look) ($start.hostSelf + 1) $start.hostPuppet $start.guestSelf ($start.guestPuppet + 1)
& $ctl -GamePid $h -Shot 'droids_1_host' | Out-Null
& $ctl -GamePid $g -Shot 'droids_1_guest' | Out-Null

# ---- 2. the guest gets a droid ----
Ask $g "script $ScriptGainItem $ItemDroid" | Out-Null
Start-Sleep $CopyWait
$hostOwn = $start.hostSelf + 1; $guestOwn = $start.guestSelf + 1
Expect 'the guest got a droid' (Look) $hostOwn $guestOwn $guestOwn $hostOwn
Check 'the guest says its droid is its own' (WaitLog $g 'familiars: own familiar' 1)
& $ctl -GamePid $h -Shot 'droids_2_host' | Out-Null
& $ctl -GamePid $g -Shot 'droids_2_guest' | Out-Null

# ---- 3. the guest's room starts again: its world makes every droid again, the copies are gone ----
Ask $g 'call room_restart' | Out-Null
Start-Sleep ($CopyWait + 2)
Expect "after the guest's room start" (Look) $hostOwn $guestOwn $guestOwn $hostOwn
& $ctl -GamePid $g -Shot 'droids_3_guest' | Out-Null

# ---- 4. the host's room starts again ----
# A bare room_restart reads the host's world from its save, where the droid given above is not yet: the host then
# has the droids of its global.droids (a real room change saves first). Its copies of the guest's must come back.
Ask $h 'call room_restart' | Out-Null
Start-Sleep ($CopyWait + 2)
$hostOwn = WorldDroids $h
Expect "after the host's room start (host world droids: $hostOwn)" (Look) $hostOwn $guestOwn $guestOwn $hostOwn
& $ctl -GamePid $h -Shot 'droids_4_host' | Out-Null
& $ctl -GamePid $g -Shot 'droids_4_guest' | Out-Null

Check 'no GML errors' (-not ((Get-Item "$env:LOCALAPPDATA\Forager\error.log" -ErrorAction SilentlyContinue).LastWriteTime -gt (Get-Process -Id $h).StartTime))
Done
