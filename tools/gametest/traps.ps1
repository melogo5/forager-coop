# PLAYTEST3 #2: fish traps are the host's. A catch the guest collects is gone at the host too (collected once, not
# once per player); a catch the host's trap makes shows in the guest's copy. Runs on the shared pair (session.ps1;
# slot 4 = a world with fish traps by the start). Usage: traps.ps1 [-Restart] [-WaitCatch 240]
param([switch]$Restart, [int]$HostSlot = 4, [int]$WaitCatch = 240)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$s = & (Join-Path $PSScriptRoot 'session.ps1') -HostSlot $HostSlot -Restart:$Restart
$h = $s.Host; $g = $s.Guest
# Traps near a game's player: id, spot, what they hold (caught[0]).
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
$hostTraps = @(Traps $h)
"HOST=$h GUEST=$g (reused: $($s.Reused)); host's traps: " + (($hostTraps | ForEach-Object { "$($_.at) holds $($_.item)" }) -join '; ')
Check "the host has traps by the start ($($hostTraps.Count))" ($hostTraps.Count -gt 0)
Check "every trap is the same at the guest" (@($hostTraps | Where-Object { (GuestTrapAt $_.at).item -ne $_.item }).Count -eq 0)

$full = $hostTraps | Where-Object { $_.item -ne '-4' } | Select-Object -First 1
if ($full) {
  $itemCount = 'script 6AB090'  # ItemCount(item): how many the asking player carries
  $hadBefore = [double](Ask $g "$itemCount $($full.item)")
  Ask $g "event $((GuestTrapAt $full.at).id) 7 11" | Out-Null  # the guest collects it (asks the host)
  Start-Sleep 2
  $afterGuest = (GuestTrapAt $full.at).item
  $afterHost = Ask $h "var $($full.id) 0xF8 0"
  $hasAfter = [double](Ask $g "$itemCount $($full.item)")
  Check "the guest collected the trap at $($full.at): empty at the guest ($afterGuest) and at the host ($afterHost)" ($afterGuest -eq '-4' -and $afterHost -eq '-4')
  Check "the catch (item $($full.item)) went into the guest's inventory: $hadBefore -> $hasAfter" ($hasAfter -gt $hadBefore)
  # A second collect of the same trap (as if a second player came a moment later) gives nothing.
  Ask $g "set $((GuestTrapAt $full.at).id) 0xF8 0 $($full.item)" | Out-Null
  Ask $g "event $((GuestTrapAt $full.at).id) 7 11" | Out-Null
  Start-Sleep 2
  $hasLate = [double](Ask $g "$itemCount $($full.item)")
  Check "collecting the emptied trap again gives nothing ($hasAfter -> $hasLate) and empties the copy ($((GuestTrapAt $full.at).item))" ($hasLate -eq $hasAfter -and (GuestTrapAt $full.at).item -eq '-4')
} else {
  'note: no trap holds a catch now - the collect check is skipped'
}

# A new catch at the host (in a trap that was empty) shows at the guest.
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
