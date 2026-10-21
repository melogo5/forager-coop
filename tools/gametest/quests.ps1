# PLAYTEST3 #4: an NPC's quest is shared. The guest talks to the first NPC (as E does: its Other_11); the host's NPC
# must end up the same - quest, state and what it waits for (itemRequirement, the "0/2") - also while it sleeps far
# from the host. Runs on the shared pair (session.ps1; slot 4 = a world with an NPC). Usage: quests.ps1 [-Restart]
param([switch]$Restart, [int]$HostSlot = 4)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$s = & (Join-Path $PSScriptRoot 'session.ps1') -HostSlot $HostSlot -Restart:$Restart
$h = $s.Host; $g = $s.Guest
function Show($npc) { if ($npc) { "$($npc.quest) / $($npc.state), needs [$($npc.requirement)]" } else { 'none' } }

# The druid comes and goes (into its tree): waited for.
function WaitNpc([int]$game) { for ($i = 0; $i -lt 15; $i++) { $n = Npc $game; if ($n) { return $n }; Start-Sleep 1 }; return $null }
$before = WaitNpc $g
"HOST=$h GUEST=$g (reused: $($s.Reused)); before: host $(Show (Npc $h)), guest $(Show $before)"
if (-not $before) { Check 'the guest has an NPC' $false; return }
Ask $g "woken 47 event $($before.id) 7 11" | Out-Null  # the guest talks to it
$guestNpc = $null; $hostNpc = $null
for ($i = 0; $i -lt 15; $i++) {  # the guest's scan (1 s), the host waking its NPC to apply it (1 s)
  Start-Sleep 1
  $guestNpc = Npc $g; $hostNpc = Npc $h
  if ($guestNpc -and $hostNpc -and (Show $hostNpc) -eq (Show $guestNpc) -and (Show $guestNpc) -ne (Show $before)) { break }
}
"after the talk: host $(Show $hostNpc), guest $(Show $guestNpc)"
Check 'the talk changed the guest''s NPC' ((Show $guestNpc) -ne (Show $before) -or $guestNpc.spoken -ne $before.spoken)
Check "the host's NPC is the same: quest, state, requirement" ($hostNpc -and (Show $hostNpc) -eq (Show $guestNpc))
