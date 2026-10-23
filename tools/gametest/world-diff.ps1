# Compares the whole world of two running games (debug port "dump": every world object, asleep ones too) and prints
# PASS when they match, else FAIL with what only one side has. Objects match when of one kind and within 4 px
# (as sync and reconcile match them). Treasure chests, interiors and moving objects are not dumped.
# Usage: world-diff.ps1 -HostPid 1234 -GuestPid 5678 [-Settle 3]   (exit code 0 = PASS, 1 = FAIL)
param([Parameter(Mandatory)][int]$HostPid, [Parameter(Mandatory)][int]$GuestPid, [int]$Settle = 0, [int]$Show = 15)
$ErrorActionPreference = 'Stop'
$query = Join-Path $PSScriptRoot 'query.ps1'
if ($Settle -gt 0) { Start-Sleep $Settle }  # a spawn or destroy just sent may still be on its way

function Dump([int]$id) {
  $answer = (& $query -GamePid $id 'dump') -replace '^dump => ', ''
  if ($answer -notmatch '^\d+:') { throw "game ${id}: no dump ($answer)" }
  $entries = @()
  foreach ($part in ($answer -replace '^\d+:', '').Split(';')) {
    $w = $part.Trim().Split(' ')
    if ($w.Count -eq 3) { $entries += [pscustomobject]@{ Object = [int]$w[0]; X = [double]$w[1]; Y = [double]$w[2]; Used = $false } }
  }
  return ,$entries
}

$hostWorld = Dump $HostPid
$guestWorld = Dump $GuestPid
$byKind = $guestWorld | Group-Object Object -AsHashTable
$hostOnly = @()
foreach ($h in $hostWorld) {
  $match = $null
  if ($byKind -and $byKind.ContainsKey($h.Object)) {
    $match = $byKind[$h.Object] | Where-Object { -not $_.Used -and [math]::Abs($_.X - $h.X) -le 4 -and [math]::Abs($_.Y - $h.Y) -le 4 } |
      Select-Object -First 1
  }
  if ($match) { $match.Used = $true } else { $hostOnly += $h }
}
$guestOnly = @($guestWorld | Where-Object { -not $_.Used })
"world: host {0} objects, guest {1}; host only {2}, guest only {3}" -f $hostWorld.Count, $guestWorld.Count, $hostOnly.Count, $guestOnly.Count
$hostOnly | Select-Object -First $Show | ForEach-Object { "  host only:  object {0} at ({1}, {2})" -f $_.Object, $_.X, $_.Y }
$guestOnly | Select-Object -First $Show | ForEach-Object { "  guest only: object {0} at ({1}, {2})" -f $_.Object, $_.X, $_.Y }
if ($hostOnly.Count -eq 0 -and $guestOnly.Count -eq 0) { 'PASS world matches'; exit 0 }
'FAIL worlds differ'; exit 1
