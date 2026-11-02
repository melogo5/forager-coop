# Skills whose purchase does more than ItemEdit/StructureEdit: the host learns 35 (pet), the guest 33 (gathering,
# +4 inventory slots) - COOP_DEBUG_SKILL, as a purchase in the tree opened over the world (no world load after it).
# Both players get a pet; the host's inventory grows by 4 (the guest's own debug "purchase" adds none). Needs a slot
# where neither skill is learned yet.
# Usage: petskills.ps1 [-Slot 5]
param([int]$Slot = 5)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$p = StartPair -Slot $Slot -HostEnv @{COOP_DEBUG_SKILL = '35'} -GuestEnv @{COOP_DEBUG_SKILL = '33'}
$h = $p.Host; $g = $p.Guest
"HOST=$h GUEST=$g"
$hostPlayer = ((Ask $h 'player') -split ' ')[0]
$slotsBefore = [double](Ask $h "var $hostPlayer 0x5A6")
Check "skill 35 not learned before" ((Ask $h 'global 0x7BF 35') -ne '2')
Check "skill 33 not learned before" ((Ask $h 'global 0x7BF 33') -ne '2')
Check "the host learned 35" (WaitLog $h 'skills: debug learned skill 35' 60)
Check "the guest learned 33" (WaitLog $g 'skills: debug learned skill 33' 60)
Check "the host made its pet" (WaitLog $h 'skills: pet \d+ made' 30)
Check "the guest made its pet" (WaitLog $g 'skills: pet \d+ made' 30)
Check "the host got the gathering slots" (WaitLog $h 'skills: gathering learned - inventory' 30)
foreach ($game in $h, $g) {
  $who = if ($game -eq $h) { 'host' } else { 'guest' }
  Check "one pet on the $who" ((Ask $game 'count 79') -eq '1')
}
$slotsAfter = [double](Ask $h "var $hostPlayer 0x5A6")
"host inventory: $slotsBefore -> $slotsAfter"
Check "host inventory +4" ($slotsAfter -eq $slotsBefore + 4)
Done
