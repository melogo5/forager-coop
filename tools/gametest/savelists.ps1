# Playtest 4 #10: a save must not get empty records (the world would never load again). Dead ids are put on
# global.herbs, the host saves, the zone file is checked. Mutant twin: -Mutant savelists must FAIL; the broken
# zone file it writes into test slot 5 is repaired at the end.
# Usage: savelists.ps1 [-Mutant savelists]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$hostEnv = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$p = StartPair -HostEnv $hostEnv
$h = $p.Host
"HOST=$h GUEST=$($p.Guest) mutant=$Mutant"
$zone = Join-Path $env:LOCALAPPDATA 'Forager\saveData5\Zones\Forager.main.cin'
$fix = Join-Path $PSScriptRoot 'cinder_fix.py'
# Dead ids on global.trueInstanceCache's parHerb list (object -> ds_list of ids; 81 = parHerb), which SaveGame walks.
# (global.herbs gets them off by itself twice a second - herbs.ps1.)
$cachedHerbs = Ask $h "call ds_map_find_value $(Ask $h 'global 0x8bd') 81"
Ask $h "call ds_list_add $cachedHerbs 5" | Out-Null
Ask $h "call ds_list_add $cachedHerbs 7" | Out-Null
$written = (Get-Item $zone).LastWriteTime
Ask $h 'script 0x004fae80' | Out-Null  # SaveGame
for ($i = 0; $i -lt 20 -and (Get-Item $zone).LastWriteTime -eq $written; $i++) { Start-Sleep 1 }
Check "the host saved the world" ((Get-Item $zone).LastWriteTime -ne $written)
Check "no GML error (a gone id stops SaveGame in TrueInstanceFind)" (-not (WaitLog $h 'crash: the game reported' 1))
$report = python -I $fix $zone
Check "the saved zone has no empty records ($report)" ($report -match ': 0 empty records')
if ($report -notmatch ': 0 empty records') {
  Get-Process Forager -ErrorAction SilentlyContinue | Stop-Process -Force
  python -I $fix $zone "$zone.fixed" | Out-Null
  Move-Item -Force "$zone.fixed" $zone
  'repaired test slot 5'
}
Done
