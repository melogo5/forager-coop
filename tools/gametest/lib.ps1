# Helpers for the check scripts (dot-source it: . (Join-Path $PSScriptRoot 'lib.ps1')).
$GameDir = 'Z:\SteamLibrary\steamapps\common\Forager'
$Query = Join-Path $PSScriptRoot 'query.ps1'

# One query-port answer (the game must run with COOP_DEBUG_PORT=1).
function Ask([int]$game, [string]$command) { ((& $Query -GamePid $game $command) -replace '^.*? => ', '') }

$script:Failed = 0
function Check([string]$name, [bool]$ok) {
  if (-not $ok) { $script:Failed++ }
  '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name
}

# A fresh host + guest pair on save slot $Slot, both with the query port; extra environment per side (a mutant:
# @{COOP_DEBUG_MUTANT = 'skills'}). Returns @{Host; Guest}.
function StartPair([hashtable]$HostEnv = @{}, [hashtable]$GuestEnv = @{}, [int]$Slot = 5) {
  Get-Process Forager -ErrorAction SilentlyContinue | Stop-Process -Force
  Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
  $r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $Slot -GodMode -GuestName 'Regress' `
    -HostEnv (@{COOP_DEBUG_PORT = '1'} + $HostEnv) -GuestEnv (@{COOP_DEBUG_PORT = '1'} + $GuestEnv)
  return @{ Host = [int]($r -replace '.*HOST=(\d+).*', '$1'); Guest = [int]($r -replace '.*GUEST=(\d+).*', '$1') }
}

# Waits (polling, up to $Seconds) until a game's log has $Pattern.
function WaitLog([int]$game, [string]$pattern, [int]$seconds) {
  for ($i = 0; $i -lt $seconds; $i++) {
    $log = "$GameDir\coop_native_$game.log"
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern $pattern -Quiet)) { return $true }
    Start-Sleep 1
  }
  return $false
}

# The test's end: exit code 1 if any check failed (mutants.ps1 counts on it).
function Done() { if ($script:Failed -gt 0) { exit 1 } else { exit 0 } }

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
