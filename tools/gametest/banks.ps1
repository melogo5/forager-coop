# Playtest 6 #3: banks are the host's. Only the host's bank earns; the guest's copy shows what it holds; coins the
# guest collects are gone at the host too (collected once, not once per player). The host builds a bank by its
# player for the check. Runs on the shared pair (session.ps1) on test slot 5. Usage: banks.ps1 [-Restart]
param([switch]$Restart, [int]$HostSlot = 5)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$s = & (Join-Path $PSScriptRoot 'session.ps1') -HostSlot $HostSlot -Restart:$Restart
$h = $s.Host; $g = $s.Guest
$coinsOf = { param($game) Ask $game "var $(((Ask $game 'player') -split ' ')[0]) 0x10C" }
function Held([int]$game, [string]$id) { Ask $game "var $id 0x146 1" }  # crafting[1]: the coins a bank holds
function GuestBank([string]$at) { [regex]::Match((Ask $g "list 125 $at 4"), '^\d+: (\d+)').Groups[1].Value }
# Makes the host's bank earn at its next HandleBanks (craftingTimer far past due), then waits for the news.
function Earn([string]$id) { Ask $h "set $id 0x14D 1000000" | Out-Null; Start-Sleep 2 }

$xy = (Ask $h 'player') -split ' '
$at = "$([double]$xy[1] + 64) $([double]$xy[2])"
$bank = Ask $h "call instance_create_depth $at 0 125"
Start-Sleep 3
$copy = GuestBank $at
"HOST=$h GUEST=$g (reused: $($s.Reused)); bank $bank at $at, the guest's copy: $copy"
Check "the guest got the host's bank ($copy)" ([bool]$copy)

Earn $bank
$hostHeld = Held $h $bank
Check "the host's bank earned ($hostHeld) and the guest's copy shows it ($(Held $g $copy))" ([double]$hostHeld -gt 0 -and (Held $g $copy) -eq $hostHeld)
Ask $g "set $copy 0x14D 1000000" | Out-Null
Start-Sleep 2
Check "the guest's bank does not earn on its own ($(Held $g $copy) = host $(Held $h $bank))" ((Held $g $copy) -eq (Held $h $bank))

$hadBefore = [double](& $coinsOf $g)
Ask $g "event $copy 7 11" | Out-Null  # the guest collects it (asks the host)
Start-Sleep 3
$hasAfter = [double](& $coinsOf $g)
Check "the guest collected: empty at the guest ($(Held $g $copy)) and at the host ($(Held $h $bank))" ((Held $g $copy) -eq '0' -and (Held $h $bank) -eq '0')
Check "the coins went to the guest: $hadBefore -> $hasAfter" ($hasAfter -gt $hadBefore)

# A second collect of the same coins (as if a second player came a moment later) gives nothing.
Ask $g "set $copy 0x146 1 $hostHeld" | Out-Null
Ask $g "event $copy 7 11" | Out-Null
Start-Sleep 3
$hasLate = [double](& $coinsOf $g)
# The copy shows the host's bank again: empty, or a coin the host's bank has earned since (one per ~600 steps).
Check "collecting the emptied bank again gives nothing ($hasAfter -> $hasLate); the copy ($(Held $g $copy)) is the host's ($(Held $h $bank))" ($hasLate -eq $hasAfter -and (Held $g $copy) -eq (Held $h $bank))

# The host collects: the guest's copy empties.
Earn $bank
Ask $h "event $bank 7 11" | Out-Null
Start-Sleep 2
Check "the host collected: empty at the host ($(Held $h $bank)) and at the guest ($(Held $g $copy))" ((Held $h $bank) -eq '0' -and (Held $g $copy) -eq '0')
Ask $h "call instance_destroy $bank" | Out-Null
Done
