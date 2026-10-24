# A join that never reaches a world can be given up: a guest connects to an address where nobody hosts, presses
# Esc on the COOP screen, and then joins the real host without restarting the game (it said "Already in a
# session - restart the game" before). The host and guest Alpha start on a test slot; the third game is the one
# under test. Prints PASS/FAIL per check. Usage: cancel.ps1 [-HostSlot 5]   (use a test slot)
param([int]$HostSlot = 5)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$exe = 'Z:\SteamLibrary\steamapps\common\Forager\Forager.exe'
$d = Split-Path $exe
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
function WaitLog([int]$id, [string]$pattern, [int]$seconds) {
  for ($i = 0; $i -lt $seconds; $i++) {
    $log = "$d\coop_native_$id.log"
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern $pattern -Quiet)) { return $true }
    Start-Sleep 1
  }
  return $false
}
function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }
function OpenCoop([int]$id) {
  for ($i = 0; $i -lt 5; $i++) {
    $before = @(Select-String -Path "$d\coop_native_$id.log" -Pattern 'menu: COOP screen opened').Count
    & $ctl -GamePid $id -Click '1060,335' -HoldMs 400 | Out-Null; Start-Sleep -Milliseconds 400
    & $ctl -GamePid $id -Click '1060,335' -HoldMs 150 | Out-Null; Start-Sleep 2
    if (@(Select-String -Path "$d\coop_native_$id.log" -Pattern 'menu: COOP screen opened').Count -gt $before) { return }
  }
  throw "game ${id}: the COOP screen did not open"
}

Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GuestName 'Alpha'
$h = [int]($r -replace '.*HOST=(\d+).*', '$1')

$id = (Start-Process -FilePath $exe -WorkingDirectory $d -PassThru).Id
if (-not (WaitLog $id 'menu: COOP button' 60)) { throw "game ${id}: no COOP button" }
Start-Sleep 4
OpenCoop $id
& $ctl -GamePid $id -Click '700,530' -HoldMs 100 | Out-Null; Start-Sleep -Milliseconds 300   # NAME field
for ($i = 0; $i -lt 20; $i++) { & $ctl -GamePid $id -Key Back -HoldMs 30 | Out-Null }
& $ctl -GamePid $id -Text 'Charlie' | Out-Null
& $ctl -GamePid $id -Key Enter | Out-Null; Start-Sleep -Milliseconds 300
& $ctl -GamePid $id -Click '700,598' -HoldMs 100 | Out-Null; Start-Sleep -Milliseconds 300   # IP field, below NAME
for ($i = 0; $i -lt 20; $i++) { & $ctl -GamePid $id -Key Back -HoldMs 30 | Out-Null }
& $ctl -GamePid $id -Text '10.255.255.1' | Out-Null
& $ctl -GamePid $id -Key Enter | Out-Null
Check 'joining an address where nobody hosts' (WaitLog $id 'session: joining 10\.255\.255\.1' 10)
Start-Sleep 3
& $ctl -GamePid $id -Key Esc | Out-Null
Check 'Esc gave the join up' (WaitLog $id 'cancelled' 10)
OpenCoop $id
function Joins { @(Select-String -Path "$d\coop_native_$id.log" -Pattern 'session: joining').Count }
$joinsBefore = Joins
for ($i = 0; $i -lt 30 -and (Joins) -eq $joinsBefore; $i++) {
  & $ctl -GamePid $id -Click '995,205' -HoldMs 150 | Out-Null; Start-Sleep 2   # JOIN on the first listed server
}
Check 'then joined the real host without a restart' (WaitLog $id 'puppet: objPlayer' 90)
Check 'the host and this guest alive' (@(Get-Process -Id $h, $id -ErrorAction SilentlyContinue).Count -eq 2)
"HOST=$h CHARLIE=$id"
