# PLAYTEST3 #1-#3: the host leaves its world (museum) and comes back - the world reloads there; the guest must not
# get it again as new objects (a second furnace on top of the first). The guest is asked (query port) how many
# furnaces, fish traps and vaults stand on each of the host's spots, before and after.
# Usage: doubles.ps1 [-HostSlotRow 1]   (a world with structures near the start: e.g. a copy of a played world)
param([int]$HostSlotRow = 1)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
$query = Join-Path $PSScriptRoot 'query.ps1'
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
Copy-Item 'native\build\version.dll' "$d\version.dll" -Force
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlotRow $HostSlotRow -GodMode `
  -HostEnv @{COOP_DEBUG_PORT = '1'; COOP_DEBUG_ROOM = '24'; COOP_DEBUG_ROOM_AT = '90'} -GuestEnv @{COOP_DEBUG_PORT = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
function Ask([int]$id, [string]$command) { ((& $query -GamePid $id $command) -replace '^.*? => ', '') }
function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }

$xy = (Ask $h 'player') -split ' '
"HOST=$h GUEST=$g; host at $($xy[1]), $($xy[2]); furnaces in the world: $(Ask $h 'count 109')"
# Every furnace, fish trap and vault near the host (structures come back from the save when the host's world
# reloads): each must stand alone on its spot at the guest. Within 160 px: farther ones sleep at the guest (culled).
function Spots() {
  $found = @()
  foreach ($object in 109, 112, 114) {  # objFurnace, objFishTrap, objVault
    $near = Ask $h "list $object $($xy[1]) $($xy[2]) 160"
    foreach ($m in [regex]::Matches($near, '\d+ ([\d.]+) ([\d.]+);')) { $found += "$object $($m.Groups[1].Value) $($m.Groups[2].Value)" }
  }
  return $found
}
function CheckSpots([string]$when) {
  $spots = Spots
  $doubled = @($spots | Where-Object { (Ask $g "at $_") -ne '1' })
  Check "${when}: $($spots.Count) structures near the host, each once at the guest$(if ($doubled) { ' - not: ' + ($doubled -join ', ') })" ($spots.Count -gt 0 -and $doubled.Count -eq 0)
}
CheckSpots 'before'

$hostLog = "$d\coop_native_$h.log"
for ($i = 0; $i -lt 120 -and -not (Select-String -Path $hostLog -Pattern 'back in the world room' -Quiet); $i++) { Start-Sleep 1 }
Check 'host went to the museum and came back' (Select-String -Path $hostLog -Pattern 'back in the world room' -Quiet)
Start-Sleep 12  # a reconcile pass or two
CheckSpots 'after'
Select-String -Path $hostLog -Pattern 'loaded from the save' | Select-Object -Last 1 | ForEach-Object { $_.Line }
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
