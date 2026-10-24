# The stopgap for broken saves (native/src/savefix.cpp): playtest 5's broken zone file (a short Entities list and empty
# records) is put into test slot 5; the host must load the world (the file repaired before loading) and the guest join.
# Mutant twin: -Mutant savefix (loaded as it is) must FAIL. Slot 5's own zone file is put back at the end.
# Usage: savefix.ps1 [-Mutant savefix]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$saves = Join-Path $env:LOCALAPPDATA 'Forager'
$zone = Join-Path $saves 'saveData5\Zones\Forager.main.cin'
$broken = Join-Path $saves 'coop_broken_20261023_saveData5\Zones\Forager.main.cin'
$fix = Join-Path $PSScriptRoot 'cinder_fix.py'
Get-Process Forager -ErrorAction SilentlyContinue | Stop-Process -Force
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
Copy-Item $zone "$zone.keep" -Force
Copy-Item $broken $zone -Force
Check "the broken file is in slot 5 ($(python -I $fix $zone | Select-Object -Last 1))" ((python -I $fix $zone | Select-Object -Last 1) -notmatch ': 0 bad records')
$extra = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$joined = $true
$started = Get-Date
try { $p = StartPair -HostEnv $extra } catch { $joined = $false }
# The host: the game started by this run that hosted slot 5 (found by its log when the guest never got in).
$h = if ($joined) { $p.Host } else {
  [int]((Get-ChildItem "$GameDir\coop_native_*.log" | Where-Object { $_.CreationTime -ge $started } |
    Where-Object { Select-String -Path $_.FullName -Pattern 'hosting save slot 5' -Quiet } |
    Select-Object -First 1).BaseName -replace 'coop_native_', '')
}
"HOST=$h mutant=$Mutant"
Check 'the host repaired the zone before loading' (WaitLog $h 'savefix: Forager.main.cin had \d+ bad records' 1)
Check 'the host loaded the world and the guest joined' $joined
Check 'no GML error at the host' (-not (WaitLog $h 'crash: the game reported' 1))
Check "the zone file reads now ($(python -I $fix $zone | Select-Object -Last 1))" ((python -I $fix $zone | Select-Object -Last 1) -match ': 0 bad records')
Get-Process Forager -ErrorAction SilentlyContinue | Stop-Process -Force
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
Move-Item -Force "$zone.keep" $zone
Remove-Item "$zone.broken" -ErrorAction SilentlyContinue
Done
