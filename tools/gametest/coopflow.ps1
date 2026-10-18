# Test helper for the native coop: starts a host and a guest through the COOP menu and waits until the
# guest is in the host's world. Prints HOST=<pid> GUEST=<pid>.
# Usage: coopflow.ps1 [-HostSlotRow 1 | -NewWorld] [-HostEnv @{COOP_DEBUG_ENEMY='26'}] [-GodMode]
param(
  [int]$HostSlotRow = 1,          # row in the save list (1 = top, the most recently played)
  [hashtable]$HostEnv = @{},
  [hashtable]$GuestEnv = @{},
  [string]$GuestName = '',        # typed into the COOP screen's NAME field before joining
  [switch]$GodMode,
  [switch]$NewWorld               # the host starts a new Classic world instead of a save
)
$ErrorActionPreference = 'Stop'
$exe = 'Z:\SteamLibrary\steamapps\common\Forager\Forager.exe'
$gameDir = Split-Path $exe
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'

function Click([int]$id, [string]$at) {
  & $ctl -GamePid $id -Click $at -HoldMs 400 | Out-Null; Start-Sleep -Milliseconds 400
  & $ctl -GamePid $id -Click $at -HoldMs 150 | Out-Null
}
function LogOf([int]$id) { Join-Path $gameDir "coop_native_$id.log" }
function WaitLog([int]$id, [string]$pattern, [int]$seconds) {
  for ($i = 0; $i -lt $seconds; $i++) {
    $log = LogOf $id
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern $pattern -Quiet)) { return $true }
    Start-Sleep 1
  }
  return $false
}
function Launch([hashtable]$envVars) {
  foreach ($k in $envVars.Keys) { Set-Item "Env:$k" $envVars[$k] }
  if ($GodMode) { $env:COOP_DEBUG_GODMODE = '1' }
  $p = Start-Process -FilePath $exe -WorkingDirectory $gameDir -PassThru
  foreach ($k in $envVars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
  Remove-Item Env:COOP_DEBUG_GODMODE -ErrorAction SilentlyContinue
  return $p.Id
}
function OpenCoop([int]$id) {
  if (-not (WaitLog $id 'menu: COOP button' 60)) { throw "game ${id}: no COOP button" }
  Start-Sleep 2
  Click $id '675,640'; Start-Sleep 2
}

$hostPid = Launch $HostEnv
OpenCoop $hostPid
& $ctl -GamePid $hostPid -Click '993,114' -HoldMs 150 | Out-Null   # CREATE
Start-Sleep 3
if ($NewWorld) {
  # The "+" button sits under the saves (100 px per save row); then the Classic mode and its PLAY button.
  $saves = @(Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Forager') -Directory -Filter 'saveData*' |
             Where-Object { $_.Name -ne 'saveData99' }).Count
  Click $hostPid ("640,{0}" -f (140 + 100 * $saves)); Start-Sleep 3
  Click $hostPid '165,370'; Start-Sleep 3
  Click $hostPid '650,575'
} else {
  Click $hostPid ("1000,{0}" -f (38 + 100 * $HostSlotRow))
}
if (-not (WaitLog $hostPid 'session: hosting' 10)) { throw 'host did not start hosting' }

$guestPid = Launch $GuestEnv
OpenCoop $guestPid
if ($GuestName) {
  & $ctl -GamePid $guestPid -Click '700,530' -HoldMs 100 | Out-Null   # NAME field
  Start-Sleep -Milliseconds 300
  for ($i = 0; $i -lt 20; $i++) { & $ctl -GamePid $guestPid -Key Back -HoldMs 30 | Out-Null }
  & $ctl -GamePid $guestPid -Text $GuestName | Out-Null
  & $ctl -GamePid $guestPid -Key Enter | Out-Null
  Start-Sleep -Milliseconds 300
}
for ($i = 0; $i -lt 30 -and -not (WaitLog $guestPid 'session: joining' 1); $i++) {
  & $ctl -GamePid $guestPid -Click '995,205' -HoldMs 150 | Out-Null   # JOIN on the first listed server
  Start-Sleep 2
}
if (-not (WaitLog $guestPid 'puppet: objPlayer' 90)) { throw 'guest did not get into the world' }
"HOST=$hostPid GUEST=$guestPid"
