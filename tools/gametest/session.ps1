# A host and a guest for checks, started once and reused: checks after the first take seconds, not the minute two
# games need to start and connect. The pair is started again only when a check says so (-Restart, or another world
# slot) or a game is gone. A rebuilt DLL does not restart it by itself: the result says Stale, and a check that
# tests the new code passes -Restart. Both games run with the query port (COOP_DEBUG_PORT=1).
# Usage: $s = session.ps1 [-HostSlot 4] [-Restart]   ->  $s.Host, $s.Guest (pids), $s.Reused, $s.Stale
param([int]$HostSlot = 4, [switch]$Restart)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$stateFile = Join-Path $PSScriptRoot 'shots\session.json'  # shots\ is not in git
$dllHash = (Get-FileHash (Join-Path $root 'native\build\version.dll') -Algorithm SHA256).Hash

function Alive([int]$id) { [bool](Get-Process -Id $id -ErrorAction SilentlyContinue) }

if (-not $Restart -and (Test-Path $stateFile)) {
  $last = Get-Content $stateFile -Raw | ConvertFrom-Json
  if ($last.slot -eq $HostSlot -and (Alive $last.host) -and (Alive $last.guest)) {
    $stale = $last.dll -ne $dllHash
    if ($stale) { Write-Warning 'the games run an older build of version.dll - pass -Restart to test the new one' }
    return [pscustomobject]@{ Host = [int]$last.host; Guest = [int]$last.guest; Reused = $true; Stale = $stale }
  }
}
& (Join-Path $PSScriptRoot 'install.ps1')
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GodMode `
  -HostEnv @{COOP_DEBUG_PORT = '1'} -GuestEnv @{COOP_DEBUG_PORT = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
New-Item -ItemType Directory -Force (Split-Path $stateFile) | Out-Null
@{ host = $h; guest = $g; slot = $HostSlot; dll = $dllHash } | ConvertTo-Json | Set-Content $stateFile
Start-Sleep 3  # the guest's world settles (first reconcile, puppets)
return [pscustomobject]@{ Host = $h; Guest = $g; Reused = $false; Stale = $false }
