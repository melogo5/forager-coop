# PLAYTEST3 #4: an NPC's quest is shared. The guest "talks" to the first NPC (COOP_DEBUG_QUEST: hasSpoken flips,
# its state is set again); the host's NPC must end up the same - also when it is far away there (kept for later).
# Needs a save with an NPC (slot 4: e.g. a copy of a played world). Usage: quests.ps1 [-HostSlot 4]
param([int]$HostSlot = 4)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
$query = Join-Path $PSScriptRoot 'query.ps1'
& (Join-Path $PSScriptRoot 'install.ps1')
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GodMode `
  -HostEnv @{COOP_DEBUG_PORT = '1'} -GuestEnv @{COOP_DEBUG_PORT = '1'; COOP_DEBUG_QUEST = '20'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
function Ask([int]$id, [string]$command) { ((& $query -GamePid $id $command) -replace '^.*? => ', '') }
function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }
# The first NPC (parNPC 47) of a game, woken up: quest, state, hasSpoken - or 'none'.
function Npc([int]$game) {
  $id = [regex]::Match((Ask $game 'woken 47 list 47'), '^\d+: (\d+)').Groups[1].Value
  if (-not $id) { return 'none' }
  return '{0} / {1} / {2}' -f (Ask $game "woken 47 var $id 0x739"), (Ask $game "woken 47 var $id 0x825"), (Ask $game "woken 47 var $id 0x55A")
}
"HOST=$h GUEST=$g"
"before: host $(Npc $h), guest $(Npc $g)"
$guestLog = "$d\coop_native_$g.log"
for ($i = 0; $i -lt 60 -and -not (Select-String -Path $guestLog -Pattern 'quests: debug touched|quests: debug: no NPC' -Quiet); $i++) { Start-Sleep 1 }
Start-Sleep 4
$touched = Select-String -Path $guestLog -Pattern 'quests: debug touched NPC \d+: now (.*)$' | Select-Object -First 1
$guestNpc = if ($touched) { $touched.Matches[0].Groups[1].Value } else { 'none' }
$hostNpc = Npc $h
"after: the guest set $guestNpc; the host has $hostNpc"
Check 'the guest touched its NPC' ([bool]$touched)
Check "the host's NPC is the same as the guest's" ($hostNpc -eq $guestNpc)
Get-Content "$d\coop_native_$h.log" | Select-String 'quests:' | Select-Object -Last 3 | ForEach-Object { $_.Line }
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
