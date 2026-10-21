# Interiors in the world room: a world save while the host is inside (SaveGame through the query port) must have
# the world as if the host stood at the door - no interior objects or layers (a layer of ours broke the world
# load once). Then run interior.ps1: a new session loads that save. Usage: interior_save.ps1
$ErrorActionPreference = 'Stop'
Set-Location 'Z:\repos\forager-coop'
. .\tools\gametest\lib.ps1
& .\tools\gametest\install.ps1
$r = & .\tools\gametest\coopflow.ps1 -HostSlot 4 -GodMode -HostEnv @{COOP_DEBUG_PORT = '1'; COOP_INWORLD_INTERIORS = '1'; COOP_DEBUG_ROOM = '23'; COOP_DEBUG_ROOM_AT = '2'} -GuestEnv @{COOP_DEBUG_PORT = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
for ($i = 0; $i -lt 30 -and -not (Select-String -Path "$GameDir\coop_native_$h.log" -Pattern 'interiors: in room' -Quiet); $i++) { Start-Sleep 1 }
"save: $(Ask $h 'script 004fae80')"
Start-Sleep 3
$hostLog = "$GameDir\coop_native_$h.log"
Get-Content $hostLog | Select-String 'interiors:|world: sent|saved as if' | ForEach-Object { $_.Line }
Check 'the host saved the world while inside, as if at the door' (Select-String -Path $hostLog -Pattern 'saved without the interior' -Quiet)
"host: $(Ask $h 'player')"
Check 'the host is still inside' ([double]((Ask $h 'player') -split ' ')[1] -gt 1700)
$zones = Get-ChildItem "$env:LOCALAPPDATA\Forager\saveData4\Zones\*.cin" | ForEach-Object { python -I tools\gametest\cinder.py $_.FullName } | Out-String
$inSave = [regex]::Matches($zones, "'(objWardrobe|objDoorBackToGame)': (\d+)") | ForEach-Object { $_.Value }
"interior objects in the saved zones: $($inSave -join ', ')"
Check 'the save has no interior objects' (-not $inSave)
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
