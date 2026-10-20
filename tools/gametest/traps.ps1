# PLAYTEST3 #2: fish traps are the host's. A catch the guest collects is gone at the host too (collected once, not
# once per player); a catch the host's trap makes shows in the guest's copy. Needs a save with fish traps by the
# start (slot 4: e.g. a copy of a played world). Usage: traps.ps1 [-HostSlot 4] [-WaitCatch 150]
param([int]$HostSlot = 4, [int]$WaitCatch = 150)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
$query = Join-Path $PSScriptRoot 'query.ps1'
& (Join-Path $PSScriptRoot 'install.ps1')
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GodMode `
  -HostEnv @{COOP_DEBUG_PORT = '1'} -GuestEnv @{COOP_DEBUG_PORT = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
function Ask([int]$id, [string]$command) { ((& $query -GamePid $id $command) -replace '^.*? => ', '') }
function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }
# Traps near a game's player: id x y, and what they hold (caught[0]).
function Traps([int]$game) {
  $xy = (Ask $game 'player') -split ' '
  [regex]::Matches((Ask $game "list 112 $($xy[1]) $($xy[2]) 400"), '(\d+) ([\d.]+) ([\d.]+);') | ForEach-Object {
    [pscustomobject]@{ id = $_.Groups[1].Value; at = "$($_.Groups[2].Value) $($_.Groups[3].Value)"; item = (Ask $game "var $($_.Groups[1].Value) 0xF8 0") }
  }
}
function GuestTrapAt([string]$at) {
  $id = [regex]::Match((Ask $g "list 112 $at 4"), '^\d+: (\d+)').Groups[1].Value
  return @{ id = $id; item = $(if ($id) { Ask $g "var $id 0xF8 0" } else { 'none' }) }
}
Start-Sleep 3
$hostTraps = @(Traps $h)
"HOST=$h GUEST=$g; host's traps: " + (($hostTraps | ForEach-Object { "$($_.at) holds $($_.item)" }) -join '; ')
Check "the host has traps by the start ($($hostTraps.Count))" ($hostTraps.Count -gt 0)

$full = $hostTraps | Where-Object { $_.item -ne '-4' } | Select-Object -First 1
if ($full) {
  $copy = GuestTrapAt $full.at
  Check "the guest's copy of the trap at $($full.at) holds the same ($($copy.item))" ($copy.item -eq $full.item)
  Ask $g "event $($copy.id) 7 11" | Out-Null  # the guest collects it
  Start-Sleep 2
  $afterGuest = (GuestTrapAt $full.at).item
  $afterHost = Ask $h "var $($full.id) 0xF8 0"
  Check "the guest collected it: empty at the guest ($afterGuest) and at the host ($afterHost)" ($afterGuest -eq '-4' -and $afterHost -eq '-4')
} else {
  'note: no trap holds a catch yet - the collect check is skipped'
}

# A new catch at the host (in a trap that was empty: the one just collected, else any) shows at the guest.
$empty = @($(if ($full) { $full.at }) + ($hostTraps | Where-Object { $_.item -eq '-4' } | ForEach-Object { $_.at }))
$caught = $null
for ($t = 0; $t -lt $WaitCatch -and -not $caught; $t += 5) {
  Start-Sleep 5
  $caught = Traps $h | Where-Object { $_.item -ne '-4' -and $empty -contains $_.at } | Select-Object -First 1
}
if ($caught) {
  Start-Sleep 2
  $copy = GuestTrapAt $caught.at
  Check "the host's trap at $($caught.at) caught $($caught.item) - the guest's copy shows it ($($copy.item))" ($copy.item -eq $caught.item)
} else {
  Check "a host trap caught something within $WaitCatch s" $false
}
$guestCatches = Get-Content "$d\coop_native_$g.log" | Select-String 'traps: collected' | Measure-Object | Select-Object -ExpandProperty Count
"guest collections logged: $guestCatches"
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
