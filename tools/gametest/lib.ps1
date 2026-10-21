# Helpers for the check scripts (dot-source it: . (Join-Path $PSScriptRoot 'lib.ps1')).
$GameDir = 'Z:\SteamLibrary\steamapps\common\Forager'
$Query = Join-Path $PSScriptRoot 'query.ps1'

# One query-port answer (the game must run with COOP_DEBUG_PORT=1).
function Ask([int]$game, [string]$command) { ((& $Query -GamePid $game $command) -replace '^.*? => ', '') }

function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }

# The first NPC (parNPC 47), woken up: id, quest, state, hasSpoken, itemRequirement - or $null.
function Npc([int]$game) {
  $id = [regex]::Match((Ask $game 'woken 47 list 47'), '^\d+: (\d+)').Groups[1].Value
  if (-not $id) { return $null }
  $requirement = @(0, 1 | ForEach-Object { Ask $game "woken 47 var $id 0x5CB $_" } | Where-Object { $_ -notmatch 'out of bounds' })
  return [pscustomobject]@{
    id = $id; quest = (Ask $game "woken 47 var $id 0x739"); state = (Ask $game "woken 47 var $id 0x825")
    spoken = (Ask $game "woken 47 var $id 0x55A"); requirement = ($requirement -join 'x')
  }
}
