# Playtest 4 #7: the host acts for a guest (its hits on rocks) with the guest's skills, then gets its own back.
# Mutant twin: -Mutant skills (the host's skills for everything) must FAIL.
# Usage: guestskills.ps1 [-Mutant skills]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$hostEnv = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$p = StartPair -HostEnv $hostEnv -GuestEnv @{COOP_DEBUG_AUTOHIT = '84'}
$h = $p.Host; $g = $p.Guest
"HOST=$h GUEST=$g mutant=$Mutant"
function HostSkills() { (0..63 | ForEach-Object { Ask $h "global 0x7BF $_" }) -join ',' }
$before = HostSkills
$applied = WaitLog $h 'guest hit object 84 .*applied' 60
Check "the guest's hits on rocks reach the host" $applied
Check "the host acted with the guest's own skills" (WaitLog $h 'skills: acting as guest \d+ with its own skills' 30)
Start-Sleep 10
Check "the host's skills are its own again after the guest's hits" ((HostSkills) -eq $before)
Done
