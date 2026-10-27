# Dungeons together (docs/coop/PLAYTEST8.md #18; dungeons.cpp). The host goes into a dungeon (COOP_DEBUG_DUNGEON,
# as an entrance would) and the guest must follow; inside, both have the same objects (dump: asleep ones too); a
# lantern the guest lights with E is lit at the host; a guest who dies gets up where it came in; 20 s later the host
# goes back out, the guest follows and both worlds are the same again (land map).
# Usage: dungeon.ps1 [-Room 14] [-HostSlot 5]
param([int]$Room = 14, [int]$HostSlot = 5)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
& (Join-Path $PSScriptRoot 'install.ps1')
$p = StartPair -Slot $HostSlot -HostEnv @{COOP_DEBUG_DUNGEON = "$Room,8"}
$h = $p.Host; $g = $p.Guest
function RoomOf([int]$game) { (Ask $game 'player').Split(' ')[3] }
for ($i = 0; $i -lt 40 -and (RoomOf $g) -ne "$Room"; $i++) { Start-Sleep 1 }
Start-Sleep 2
Check "both went into room $Room (host $(RoomOf $h), guest $(RoomOf $g))" ((RoomOf $h) -eq "$Room" -and (RoomOf $g) -eq "$Room")

# Objects per kind, asleep ones too ("N: object x y; ...").
function Kinds([int]$game) {
  $counts = @{}
  foreach ($m in [regex]::Matches((Ask $game 'dump'), '(\d+) [\d.-]+ [\d.-]+;')) { $counts[$m.Groups[1].Value]++ }
  $counts
}
$hk = Kinds $h; $gk = Kinds $g
$differ = @($hk.Keys + $gk.Keys | Sort-Object -Unique | Where-Object { $hk[$_] -ne $gk[$_] } |
  ForEach-Object { "$_ host $([int]$hk[$_]) guest $([int]$gk[$_])" })
Check "the same objects at both ($($differ.Count) kinds differ$(if ($differ) { ': ' + ($differ -join '; ') }))" ($differ.Count -eq 0)

$lantern = [regex]::Match((Ask $g 'list 207'), '^\d+: (\d+) ([\d.]+) ([\d.]+)')
if ($lantern.Success) {
  Ask $g "event $($lantern.Groups[1].Value) 7 11" | Out-Null
  Start-Sleep 2
  $at = "$($lantern.Groups[2].Value) $($lantern.Groups[3].Value)"
  $hostLantern = [regex]::Matches((Ask $h 'list 207'), '(\d+) ([\d.]+ [\d.]+);') | Where-Object { $_.Groups[2].Value -eq $at }
  $lit = if ($hostLantern) { Ask $h "var $($hostLantern[0].Groups[1].Value) 0x610" } else { 'none there' }
  Check "a lantern the guest lit is lit at the host ($lit)" ($lit -eq '1')
} else { 'note: no lantern in this room' }

# A cube the guest pushes (E): the host pushes it as the guest's puppet; the guest's cube stands where the host's does.
$cube = [regex]::Match((Ask $g 'list 193'), '^\d+: (\d+) ([\d.]+) ([\d.]+)')
if ($cube.Success) {
  $id = $cube.Groups[1].Value
  $before = (Ask $h "obj $id")
  Ask $g "event $id 7 11" | Out-Null
  Start-Sleep 3
  $hostCube = Ask $h "obj $id"; $guestCube = Ask $g "obj $id"
  "  cube ${id}: before $before; host now $hostCube; guest now $guestCube"
  Check 'a cube the guest pushed moved at the host' ($hostCube -ne $before)
  Check 'the guest sees the cube where the host has it' ($hostCube -eq $guestCube)
} else { 'note: no cube in this room' }

$hpSlot =(Select-String (Join-Path $PSScriptRoot '..\..\native\src\yyc.h') -Pattern 'constexpr int hp = (0x[0-9A-Fa-f]+|\d+)').Matches[0].Groups[1].Value
$me = (Ask $g 'player').Split(' ')
Ask $g "set $($me[0]) $hpSlot 0" | Out-Null
Start-Sleep 1
$after = (Ask $g 'player').Split(' ')
Check "the guest died and got up in the dungeon (room $($after[3]), hp $(Ask $g "var $($after[0]) $hpSlot"))" `
  ($after[3] -eq "$Room" -and [double](Ask $g "var $($after[0]) $hpSlot") -ge 1)

for ($i = 0; $i -lt 40 -and (RoomOf $g) -ne '8'; $i++) { Start-Sleep 1 }
Start-Sleep 8  # the guest's world loads its zones a while after the room
Check "both back in the world (host $(RoomOf $h), guest $(RoomOf $g))" ((RoomOf $h) -eq '8' -and (RoomOf $g) -eq '8')
$sameLand = (Ask $h 'watermap 8 8 1336 1336 16') -eq (Ask $g 'watermap 8 8 1336 1336 16')
Check 'the land is the same at both again' $sameLand
Check 'no GML errors in the logs' (-not (Select-String "$GameDir\coop_native_$h.log", "$GameDir\coop_native_$g.log" -Pattern 'FATAL|crash:' -Quiet))
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Done
