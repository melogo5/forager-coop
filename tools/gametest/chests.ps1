# Playtest 4 #9: treasure chests are each player's own. A chest the guest opens stays at the host; one the host opens
# stays at the guest. Mutant twin: -Mutant chests (the old rule: gone for everyone) must FAIL.
# Usage: chests.ps1 [-Mutant chests]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$hostEnv = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$p = StartPair -HostEnv $hostEnv
$h = $p.Host; $g = $p.Guest
"HOST=$h GUEST=$g mutant=$Mutant"
$xy = (Ask $h 'player') -split ' '
# A new spot every run: the guest's list of chests it opened (coop_guests/<name>.chests) hides any chest standing
# where one it opened stood.
$x = [double]$xy[1] + 16 * (Get-Random -Minimum 2 -Maximum 6); $y = [double]$xy[2] + 16 * (Get-Random -Minimum -3 -Maximum 3)
# Chests left at the spot by earlier runs (the host keeps one the guest opened) would make new ones slide off them.
foreach ($game in $h, $g) {
  foreach ($m in [regex]::Matches((Ask $game "list 151 $x $($y + 18) 80"), '(\d+) [\d.]+ [\d.]+;')) {
    Ask $game "call instance_destroy $($m.Groups[1].Value) 0" | Out-Null
  }
}
Start-Sleep 1
function ChestAt([int]$game, [double]$cx, [double]$cy) {
  [regex]::Match((Ask $game "list 151 $cx $cy 4"), '^\d+: (\d+)').Groups[1].Value
}

# The guest opens one: gone at the guest, kept at the host.
function WaitChest([int]$game, [double]$cx, [double]$cy, [bool]$there, [int]$seconds = 15) {
  for ($i = 0; $i -lt $seconds; $i++) {
    if ([bool](ChestAt $game $cx $cy) -eq $there) { return $true }
    Start-Sleep 1
  }
  return $false
}

# The guest opens one: gone at the guest, kept at the host. Each step counts only once the one before it happened.
Ask $h "call instance_create_depth $x $y 0 151" | Out-Null
$reached = WaitChest $g $x $y $true
Check "a chest the host spawned reached the guest" $reached
if ($reached) {
  Ask $g "event $(ChestAt $g $x $y) 7 11" | Out-Null
  Check "the guest opened it: gone at the guest" (WaitChest $g $x $y $false)
  Start-Sleep 3  # the news reaches the host
  Check "... and still there at the host" ([bool](ChestAt $h $x $y))
}

# The host opens another: gone at the host, kept at the guest.
$y2 = $y + 36
$hostChest = (Ask $h "call instance_create_depth $x $y2 0 151")
$reached = WaitChest $g $x $y2 $true
Check "a second chest reached the guest" $reached
if ($reached) {
  Ask $h "event $hostChest 7 11" | Out-Null
  Check "the host opened its chest: gone at the host" (WaitChest $h $x $y2 $false)
  Start-Sleep 3
  Check "... and still there at the guest" ([bool](ChestAt $g $x $y2))
}
Done
