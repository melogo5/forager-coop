# Playtest 4 #10: a dead id on global.herbs stops the game (HerbProcess -> IsDeactivated, "UH OH"); the DLL takes
# such ids off twice a second. A dead id is put on the host's list; the game must keep going. Mutant twin:
# -Mutant herbs (the ids stay) must FAIL.
# Usage: herbs.ps1 [-Mutant herbs]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$hostEnv = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$p = StartPair -HostEnv $hostEnv
$h = $p.Host
"HOST=$h GUEST=$($p.Guest) mutant=$Mutant"
$herbs = Ask $h 'global 0x55f'
Ask $h "call ds_list_add $herbs 5" | Out-Null
Start-Sleep 8
Check 'the dead herb id is taken off the list' (WaitLog $h 'world: herb 5 is gone but still listed' 1)
Check 'the game goes on (no GML error)' (-not (WaitLog $h 'crash: the game reported' 1))
Done
