# Test helper for 3-4 player sessions: starts one more guest, which joins the first listed server through the
# COOP menu under -Name, and waits until it is in the world. Prints GUEST=<pid>.
# Usage (after coopflow.ps1 started the host and the first guest): joinguest.ps1 -Name Third [-GuestEnv @{...}]
param(
  [Parameter(Mandatory)][string]$Name,
  [hashtable]$GuestEnv = @{}
)
$ErrorActionPreference = 'Stop'
$exe = 'Z:\SteamLibrary\steamapps\common\Forager\Forager.exe'
$gameDir = Split-Path $exe
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
function WaitLog([int]$id, [string]$pattern, [int]$seconds) {
  for ($i = 0; $i -lt $seconds; $i++) {
    $log = Join-Path $gameDir "coop_native_$id.log"
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern $pattern -Quiet)) { return $true }
    Start-Sleep 1
  }
  return $false
}

foreach ($k in $GuestEnv.Keys) { Set-Item "Env:$k" $GuestEnv[$k] }
$id = (Start-Process -FilePath $exe -WorkingDirectory $gameDir -PassThru).Id
foreach ($k in $GuestEnv.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
if (-not (WaitLog $id 'menu: COOP button' 60)) { throw "guest ${id}: no COOP button" }
Start-Sleep 4   # the button is listed before the menu takes clicks
for ($i = 0; $i -lt 5 -and -not (WaitLog $id 'menu: COOP screen opened' 1); $i++) {
  & $ctl -GamePid $id -Click '675,640' -HoldMs 400 | Out-Null; Start-Sleep -Milliseconds 400
  & $ctl -GamePid $id -Click '675,640' -HoldMs 150 | Out-Null; Start-Sleep 2
}
& $ctl -GamePid $id -Click '700,530' -HoldMs 100 | Out-Null; Start-Sleep -Milliseconds 300   # NAME field
for ($i = 0; $i -lt 20; $i++) { & $ctl -GamePid $id -Key Back -HoldMs 30 | Out-Null }
& $ctl -GamePid $id -Text $Name | Out-Null
& $ctl -GamePid $id -Key Enter | Out-Null; Start-Sleep -Milliseconds 300
for ($i = 0; $i -lt 30 -and -not (WaitLog $id 'session: joining' 1); $i++) {
  & $ctl -GamePid $id -Click '995,205' -HoldMs 150 | Out-Null; Start-Sleep 2   # JOIN on the first listed server
}
if (-not (WaitLog $id 'puppet: objPlayer' 90)) { throw "guest ${id} did not get into the world" }
"GUEST=$id"
