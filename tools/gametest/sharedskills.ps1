# One skill tree for everyone: the host and the guest each learn a different skill not learned yet
# (COOP_DEBUG_SKILL=avail0 / avail1); both trees end up with both learned. Mutant twin: -Mutant skills (a tree from
# the other player is ignored) must FAIL.
# Usage: sharedskills.ps1 [-Mutant skills]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$mutantEnv = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$p = StartPair -HostEnv (@{COOP_DEBUG_SKILL = 'avail0'} + $mutantEnv) -GuestEnv (@{COOP_DEBUG_SKILL = 'avail1'} + $mutantEnv)
$h = $p.Host; $g = $p.Guest
"HOST=$h GUEST=$g mutant=$Mutant"
function Learned([int]$game) {
  if (-not (WaitLog $game 'skills: debug learned skill \d+' 60)) { return -1 }
  $line = Select-String -Path "$GameDir\coop_native_$game.log" -Pattern 'skills: debug learned skill (\d+)' | Select-Object -First 1
  return [int]$line.Matches[0].Groups[1].Value
}
$hostSkill = Learned $h; $guestSkill = Learned $g
"host learned $hostSkill, guest learned $guestSkill"
Check "each learned a skill of its own" ($hostSkill -ge 0 -and $guestSkill -ge 0 -and $hostSkill -ne $guestSkill)
Start-Sleep 5
foreach ($game in $h, $g) {
  $who = if ($game -eq $h) { 'host' } else { 'guest' }
  Check "the host's skill $hostSkill is learned on the $who" ((Ask $game "global 0x7BF $hostSkill") -eq '2')
  Check "the guest's skill $guestSkill is learned on the $who" ((Ask $game "global 0x7BF $guestSkill") -eq '2')
}
Done
