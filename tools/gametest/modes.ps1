# COOP -> CREATE -> "+" shows only Classic, and Classic still starts a world. The new world's slot is removed at the
# end. Mutant twin: -Mutant modes (every mode shown) must FAIL.
# Usage: modes.ps1 [-Mutant modes]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
$saves = Join-Path $env:LOCALAPPDATA 'Forager'
Get-Process Forager -ErrorAction SilentlyContinue | Stop-Process -Force
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
$before = @(Get-ChildItem $saves -Directory -Filter 'saveData*' | ForEach-Object Name)
if ($Mutant) { $env:COOP_DEBUG_MUTANT = $Mutant }
$id = (Start-Process -FilePath "$GameDir\Forager.exe" -WorkingDirectory $GameDir -PassThru).Id
Remove-Item Env:COOP_DEBUG_MUTANT -ErrorAction SilentlyContinue
"GAME=$id mutant=$Mutant"
function Click([string]$at) { & $ctl -GamePid $id -Click $at -HoldMs 400 | Out-Null; Start-Sleep -Milliseconds 400; & $ctl -GamePid $id -Click $at -HoldMs 150 | Out-Null }
WaitLog $id 'menu: COOP button' 60 | Out-Null
for ($i = 0; $i -lt 10 -and -not (WaitLog $id 'COOP screen opened' 1); $i++) { Start-Sleep 1; & $ctl -GamePid $id -Click '1060,335' -HoldMs 150 | Out-Null }
for ($i = 0; $i -lt 4 -and -not (WaitLog $id 'session: hosting' 2); $i++) { & $ctl -GamePid $id -Click '993,114' -HoldMs 150 | Out-Null }
Start-Sleep 2
$rows = @($before | Where-Object { $_ -ne 'saveData99' }).Count
Click ("640,{0}" -f (140 + 100 * $rows))  # "+": a new world
Check 'the mode screen shows Classic only' (WaitLog $id 'menu: mode screen for a coop world - 3 modes other than Classic hidden' 10)
Click '165,370'; Start-Sleep 3
& $ctl -GamePid $id -Click '650,575' -HoldMs 150 | Out-Null  # PLAY
Check 'Classic starts a coop world' (WaitLog $id 'session: in the world' 60)
Get-Process -Id $id -ErrorAction SilentlyContinue | Stop-Process -Force
Wait-Process -Id $id -Timeout 30 -ErrorAction SilentlyContinue
foreach ($slot in @(Get-ChildItem $saves -Directory -Filter 'saveData*' | ForEach-Object Name | Where-Object { $before -notcontains $_ })) {
  Remove-Item -Recurse -Force (Join-Path $saves $slot)
  "removed the test world $slot"
}
Done
