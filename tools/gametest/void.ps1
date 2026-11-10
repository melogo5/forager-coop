# The Void together (docs/coop/VOID.md; voidzone.cpp). The host enters the Void (VoidLoad, as its portal does): the
# guest follows into the same level - same land, same enemies, the host's timer, the level not over while the host
# still has enemies (playtest 11: the guest was left in the world with the Void's enemies; with both inside, the
# guest's timer stood and its portal on was there at once). Every enemy killed: the level is complete at both and the
# portal on is there; the guest's E on it takes everyone to level 2. The timer at 0: the leaderboard at both; the
# guest's way back (its button) takes everyone back to the world, the same world at both.
# Mutant twin: -Mutant void (the Void as before: nothing of voidzone.cpp) must FAIL.
# Usage: void.ps1 [-HostSlot 4] [-Mutant void]
param([int]$HostSlot = 4, [string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
& (Join-Path $PSScriptRoot 'install.ps1')
$errorLog = Join-Path $env:LOCALAPPDATA 'Forager\error.log'
$errorsBefore = if (Test-Path $errorLog) { (Get-Item $errorLog).LastWriteTime } else { $null }
$env = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$p = StartPair -Slot $HostSlot -HostEnv $env -GuestEnv $env
$h = $p.Host; $g = $p.Guest
"HOST=$h GUEST=$g mutant=$Mutant"

function InVoid([int]$game) { (Ask $game 'script 834630') -eq '1' }
function Level([int]$game) { Ask $game 'global 0x528' }
function Control([int]$game) { [regex]::Match((Ask $game 'list 4'), '^\d+: (\d+)').Groups[1].Value }
function ControlVar([int]$game, [string]$slot) { $c = Control $game; if ($c) { [double](Ask $game "var $c $slot") } else { $null } }
function WaitBoth([scriptblock]$ok, [int]$seconds) {
  for ($i = 0; $i -lt $seconds; $i++) { if (& $ok) { return $true }; Start-Sleep 1 }
  return $false
}
# Objects per kind, asleep ones too.
function Kinds([int]$game) {
  $counts = @{}
  foreach ($m in [regex]::Matches((Ask $game 'dump'), '(\d+) [\d.-]+ [\d.-]+;')) { $counts[$m.Groups[1].Value]++ }
  $counts
}
function Differ($a, $b, [scriptblock]$which) {
  @($a.Keys + $b.Keys | Sort-Object -Unique | Where-Object { & $which ([int]$_) } | Where-Object { $a[$_] -ne $b[$_] } |
    ForEach-Object { "$_ host $([int]$a[$_]) guest $([int]$b[$_])" })
}
$anyKind = { param($o) $true }
function LandDiff {
  $hm = Ask $h 'watermap 0 0 1336 1336 16'; $gm = Ask $g 'watermap 0 0 1336 1336 16'
  $d = 0; for ($i = 0; $i -lt [math]::Min($hm.Length, $gm.Length); $i++) { if ($hm[$i] -ne $gm[$i]) { $d++ } }
  $d
}

# 1. Into the Void.
"VoidLoad at the host: $(Ask $h 'script 73d4f0')"
$both = WaitBoth { (InVoid $h) -and (InVoid $g) -and (Level $g) -eq '1' } 40
Check "both are in the Void, level 1 (host $(InVoid $h) level $(Level $h), guest $(InVoid $g) level $(Level $g))" $both
Start-Sleep 2
$start = (Ask $g 'player').Split(' ')  # id x y room: where the level put the guest
$hostStart = (Ask $h 'player').Split(' ')
Check "the guest starts where the host does (guest $($start[1]),$($start[2]), host $($hostStart[1]),$($hostStart[2]))" ($start[1] -eq $hostStart[1] -and $start[2] -eq $hostStart[2])
Start-Sleep 4  # the host's land map goes out ~2 s after the level is made
$land = LandDiff
Check "the same land at both ($land tiles differ)" ($land -le 4)
$hk = Kinds $h; $gk = Kinds $g
$he = [int](Ask $h 'woken 23 count 23'); $ge = [int](Ask $g 'woken 23 count 23')  # every enemy (parEnemy), asleep too
Check "the same enemies at both (host $he, guest $ge)" ($he -gt 0 -and $he -eq $ge)
$all = Differ $hk $gk $anyKind
Check "the same objects at both ($($all.Count) kinds differ$(if ($all) { ': ' + ($all -join '; ') }))" ($all.Count -le 1)
$ht = ControlVar $h 0x888; $gt1 = ControlVar $g 0x888; Start-Sleep 3; $gt2 = ControlVar $g 0x888
Check "the guest's timer is the host's and runs (host $ht, guest $gt1 -> $gt2)" ($null -ne $gt1 -and [math]::Abs($gt1 - $ht) -lt 120 -and $gt2 -lt $gt1)
Check "with enemies left the level is not over at the guest (complete $(ControlVar $g 0x604), portals $(Ask $g 'count 148'))" ((ControlVar $g 0x604) -eq 0 -and (Ask $g 'count 148') -eq '0')

# A guest who dies in the Void gets up where the level began - still in it, no leaderboard (the run goes on).
$me = (Ask $g 'player').Split(' ')
Ask $g "moveto $($me[0]) $([double]$start[1] + 48) $($start[2])" | Out-Null
Ask $g "set $($me[0]) 0x573 0" | Out-Null
$up = WaitLog $g 'died - respawned at the' 5
$now = (Ask $g 'player').Split(' ')
$fromStart = [math]::Round([math]::Sqrt([math]::Pow([double]$now[1] - [double]$start[1], 2) + [math]::Pow([double]$now[2] - [double]$start[2], 2)))
Check "a guest who died got up where the level began ($fromStart px from it; void $(InVoid $g), level $(Level $g), leaderboards $(Ask $g 'count 268'))" ($up -and $fromStart -le 8 -and (InVoid $g) -and (Level $g) -eq '1' -and (Ask $g 'count 268') -eq '0')

# 2. Every enemy killed at the host: the level is complete at both, the portal on is there.
foreach ($m in [regex]::Matches((Ask $h 'woken 23 list 23'), '(\d+) [\d.-]+ [\d.-]+')) {
  Ask $h "woken 23 call instance_destroy $($m.Groups[1].Value)" | Out-Null
}
$done = WaitBoth { (ControlVar $h 0x604) -eq 1 -and (ControlVar $g 0x604) -eq 1 -and [int](Ask $g 'count 148') -gt 0 } 20
Check "no enemy left: complete at both, the portal at the guest (host $(ControlVar $h 0x604), guest $(ControlVar $g 0x604), portals $(Ask $g 'count 148'))" $done

# 3. The guest's E on the portal: everyone to level 2.
$portal = [regex]::Match((Ask $g 'list 148'), '^\d+: (\d+) ([\d.-]+) ([\d.-]+)')
if ($portal.Success) {  # its Other_11 wants the player close by: the guest walks up to it first
  $me = (Ask $g 'player').Split(' ')
  Ask $g "moveto $($me[0]) $($portal.Groups[2].Value) $([double]$portal.Groups[3].Value + 24)" | Out-Null
  Start-Sleep 1
  Ask $g "event $($portal.Groups[1].Value) 7 11" | Out-Null
}
$two = WaitBoth { (Level $h) -eq '2' -and (Level $g) -eq '2' -and (InVoid $h) -and (InVoid $g) } 40
Check "the guest's portal took both to level 2 (host $(Level $h), guest $(Level $g))" $two
Start-Sleep 6
Check "level 2: the same land at both ($(LandDiff) tiles differ)" ((LandDiff) -le 4)

# 4. The timer runs out: the leaderboard at both; the guest's way back takes everyone to the world.
$c = Control $h
if ($c) { Ask $h "set $c 0x888 60" | Out-Null }
$over = WaitBoth { [int](Ask $h 'count 268') -gt 0 -and [int](Ask $g 'count 268') -gt 0 } 15
Check "the timer ran out: the leaderboard at both (host $(Ask $h 'count 268'), guest $(Ask $g 'count 268'))" $over
"the guest's button: $(Ask $g 'script 73a1a0 0')"
$back = WaitBoth { -not (InVoid $h) -and -not (InVoid $g) -and (Ask $h 'player').Split(' ')[3] -eq '8' -and (Ask $g 'player').Split(' ')[3] -eq '8' } 40
Check "both back in the world (host void $(InVoid $h), guest void $(InVoid $g))" $back
Start-Sleep 12  # a census or two
$world = Differ (Kinds $h) (Kinds $g) $anyKind
Check "the same world at both ($($world.Count) kinds differ$(if ($world) { ': ' + (($world | Select-Object -First 6) -join '; ') }))" ($world.Count -le 1)
$errorsAfter = if (Test-Path $errorLog) { (Get-Item $errorLog).LastWriteTime } else { $null }
Check 'no GML errors (error.log unchanged)' ($errorsAfter -eq $errorsBefore)
Check 'both games alive' ((Get-Process -Id $h, $g -ErrorAction SilentlyContinue).Count -eq 2)
Get-Process Forager -ErrorAction SilentlyContinue | Stop-Process -Force
Done
