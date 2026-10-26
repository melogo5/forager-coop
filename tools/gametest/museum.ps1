# The museum (playtest 6: every shelf stood empty, and donations were never shared).
# 1. The host goes into the museum (built inside the world room): the 8 bundles got their number from the room's
#    instance creation code, and every showcase shows a bundle that is there (its `owner`).
# 2. Two donations within the same second both stay: the host and the guest each put one more item into a different
#    bundle of global.museumBundles (a ds_grid: column = bundle, row = item) through the query port; a few seconds
#    later both grids hold both, and then nothing goes back and forth.
# Run on a test slot (the donations stay in its museum). Prints PASS/FAIL per check. Usage: museum.ps1 [-HostSlot 5]
param([int]$HostSlot = 5)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
. (Join-Path $PSScriptRoot 'lib.ps1')

$pair = StartPair -Slot $HostSlot -HostEnv @{COOP_DEBUG_ROOM = '24'; COOP_DEBUG_ROOM_AT = '10'}
$h = $pair.Host; $g = $pair.Guest
"HOST=$h GUEST=$g"

Check 'the host went into the museum' (WaitLog $h 'interiors: in room 24 inside the world room' 60)
Start-Sleep 2
$bundles = @([regex]::Matches((Ask $h 'list 172'), '(\d+) [\d.]+ [\d.]+;') | ForEach-Object { $_.Groups[1].Value })
$numbers = @($bundles | ForEach-Object { [int](Ask $h "var $_ 0x6AF") } | Sort-Object)
Check "the 8 bundles are numbered 0..7 ($($numbers -join ','))" (($numbers -join ',') -eq '0,1,2,3,4,5,6,7')
$showcases = @([regex]::Matches((Ask $h 'list 173'), '(\d+) [\d.]+ [\d.]+;') | ForEach-Object { $_.Groups[1].Value })
$owners = @($showcases | ForEach-Object { Ask $h "var $_ 0x6CA" })
$lost = @($owners | Where-Object { $bundles -notcontains $_ }).Count
Check "every one of the $($showcases.Count) showcases shows a bundle that is here ($lost do not)" ($showcases.Count -eq 69 -and $lost -eq 0)

function Cell([int]$game, [int]$x, [int]$y) { [double](Ask $game "call ds_grid_get $(Ask $game 'global 0x68A') $x $y") }
function Give([int]$game, [int]$x) {
  $id = Ask $game 'global 0x68A'
  Ask $game "call ds_grid_set $id $x 0 $((Cell $game $x 0) + 1)" | Out-Null
}
$a = Cell $h 0 0; $b = Cell $h 1 0
Give $h 0
Give $g 1
Start-Sleep 5
Check "the host's museum has both donations (bundle 0: $a -> $(Cell $h 0 0), bundle 1: $b -> $(Cell $h 1 0))" ((Cell $h 0 0) -eq $a + 1 -and (Cell $h 1 0) -eq $b + 1)
Check "the guest's museum has both donations (bundle 0: $(Cell $g 0 0), bundle 1: $(Cell $g 1 0))" ((Cell $g 0 0) -eq $a + 1 -and (Cell $g 1 0) -eq $b + 1)
function Sends { @(Get-Content "$GameDir\coop_native_$h.log", "$GameDir\coop_native_$g.log" | Select-String 'museum changed here').Count }
$sends = Sends; Start-Sleep 5
Check "then they stop sending it (no back and forth; $sends sends)" ((Sends) -eq $sends -and $sends -ge 2)
Check 'no GML errors' (-not ((Get-Item "$env:LOCALAPPDATA\Forager\error.log" -ErrorAction SilentlyContinue).LastWriteTime -gt (Get-Process -Id $h).StartTime))
Done
