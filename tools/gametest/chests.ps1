# Playtest 5 #3: a key chest opens once every player has put a key in, and then everyone gets the same reward; a
# keyless chest opened by one player opens for everyone. Mutant twin: -Mutant chests (the game's own: one key opens it
# for its opener only) must FAIL.
# Usage: chests.ps1 [-Mutant chests]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$hostEnv = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$p = StartPair -HostEnv $hostEnv -GuestEnv $hostEnv
$h = $p.Host; $g = $p.Guest
"HOST=$h GUEST=$g mutant=$Mutant"
$gainItem = '0x006ac690'; $itemCount = '0x006ab090'; $key = 110
$xy = (Ask $h 'player') -split ' '
$x = [double]$xy[1] + 16 * (Get-Random -Minimum 2 -Maximum 6); $y = [double]$xy[2] + 16 * (Get-Random -Minimum -3 -Maximum 3)
# Chests left near the spot by earlier runs would make new ones slide onto them (and be taken for them).
foreach ($game in $h, $g) {
  foreach ($object in 150, 151) {
    foreach ($m in [regex]::Matches((Ask $game "list $object $x $($y + 48) 200"), '(\d+) [\d.]+ [\d.]+;')) {
      Ask $game "call instance_destroy $($m.Groups[1].Value) 0" | Out-Null
    }
  }
}
Start-Sleep 1
function ChestAt([int]$game, [int]$object, [double]$cx, [double]$cy) {
  [regex]::Match((Ask $game "list $object $cx $cy 64"), '^\d+: (\d+)').Groups[1].Value
}
function Closed([int]$game, [int]$object, [double]$cx, [double]$cy) {
  $id = ChestAt $game $object $cx $cy
  return [bool]$id -and (Ask $game "var $id 0x8CA") -ne '0'
}
function WaitFor([scriptblock]$condition, [int]$seconds = 15) {
  for ($i = 0; $i -lt $seconds; $i++) { if (& $condition) { return $true }; Start-Sleep 1 }
  return $false
}

# Keyless: the guest opens it - it opens at the host too.
Ask $h "call instance_create_depth $x $y 0 151" | Out-Null
$reached = WaitFor { [bool](ChestAt $g 151 $x $y) }
Check 'a keyless chest the host spawned reached the guest' $reached
if ($reached) {
  Ask $g "event $(ChestAt $g 151 $x $y) 7 11" | Out-Null
  Check '... the guest opened it - it opened at the host too' (WaitFor { -not (Closed $h 151 $x $y) })
}

# Key chest: a key each; the guest's key alone does not open it, the host's second one opens it for both.
$y2 = $y + 96  # far enough not to land on the first one
Ask $h "call instance_create_depth $x $y2 0 150" | Out-Null
$reached = WaitFor { [bool](ChestAt $g 150 $x $y2) }
Check 'a key chest the host spawned reached the guest' $reached
if ($reached) {
  $keysBefore = "$(Ask $h "script $itemCount $key")/$(Ask $g "script $itemCount $key")"  # earlier runs may have left some
  Ask $h "script $gainItem $key" | Out-Null
  Ask $g "script $gainItem $key" | Out-Null
  Start-Sleep 1
  Ask $g "event $(ChestAt $g 150 $x $y2) 7 11" | Out-Null
  Start-Sleep 3
  Check "after the guest's key it is still closed at both (keys 1/2)" ((Closed $h 150 $x $y2) -and (Closed $g 150 $x $y2))
  Ask $h "event $(ChestAt $h 150 $x $y2) 7 11" | Out-Null
  Check "after the host's key it opened at both" (WaitFor { -not (Closed $h 150 $x $y2) -and -not (Closed $g 150 $x $y2) })
  $hostReward = [regex]::Match((Get-Content "$GameDir\coop_native_$h.log" -Raw), 'opened for everyone, reward item (-?\d+)').Groups[1].Value
  $guestReward = [regex]::Match((Get-Content "$GameDir\coop_native_$g.log" -Raw), 'opened for everyone - reward item (-?\d+)').Groups[1].Value
  Check "both got the same reward (host $hostReward, guest $guestReward)" ($hostReward -and $hostReward -ne '-1' -and $hostReward -eq $guestReward)
  $keysAfter = "$(Ask $h "script $itemCount $key")/$(Ask $g "script $itemCount $key")"
  Check "both keys are used up (keys before $keysBefore, after $keysAfter)" ($keysAfter -eq $keysBefore)
}
Done
