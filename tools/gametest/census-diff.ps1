# Compares the host's and the guest's world objects written by COOP_DEBUG_CENSUS (coop_census_<pid>_<n>.txt).
# Lines: "<object> <x> <y> <host id>". Prints what only one side has.
# Usage: census-diff.ps1 -HostPid 1234 -GuestPid 5678 [-Number 3]   (default: the latest census both wrote)
param(
  [Parameter(Mandatory)][int]$HostPid,
  [Parameter(Mandatory)][int]$GuestPid,
  [int]$Number = 0
)
$ErrorActionPreference = 'Stop'
$gameDir = 'Z:\SteamLibrary\steamapps\common\Forager'

function CensusNumbers([int]$id) {
  Get-ChildItem $gameDir -Filter "coop_census_${id}_*.txt" |
    ForEach-Object { [int]($_.BaseName -replace "^coop_census_${id}_", '') }
}
if ($Number -eq 0) {
  $common = CensusNumbers $HostPid | Where-Object { (CensusNumbers $GuestPid) -contains $_ }
  if (-not $common) { throw 'no census written by both sides yet' }
  $Number = ($common | Measure-Object -Maximum).Maximum
}

# Object + position identify a world object on both sides (host ids are unknown for objects from the save).
function Keys([int]$id) {
  Get-Content (Join-Path $gameDir "coop_census_${id}_$Number.txt") |
    ForEach-Object { $f = $_ -split ' '; "$($f[0]) $($f[1]) $($f[2])" }
}
$hostKeys = Keys $HostPid
$guestKeys = Keys $GuestPid
"census #${Number}: host $($hostKeys.Count) objects, guest $($guestKeys.Count)"
Compare-Object $hostKeys $guestKeys | Sort-Object InputObject | ForEach-Object {
  $side = if ($_.SideIndicator -eq '<=') { 'host only ' } else { 'guest only' }
  "$side  object/x/y: $($_.InputObject)"
}
