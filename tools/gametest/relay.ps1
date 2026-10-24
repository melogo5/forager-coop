# Three players: what one guest builds reaches the other guest through the host (REVIEW.md: the host built it
# quietly and told nobody else). The host and guest Alpha start on a test slot; guest Bravo joins and builds
# object 109 next to itself ~5 s into the world; Alpha must then have it at the same spot.
# Prints PASS/FAIL per check. Usage: relay.ps1 [-HostSlot 5] [-Seconds 20]   (use a test slot)
param([int]$HostSlot = 5, [int]$Seconds = 20)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
$query = Join-Path $PSScriptRoot 'query.ps1'

Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -GuestName 'Alpha' -GuestEnv @{COOP_DEBUG_PORT = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g1 = [int]($r -replace '.*GUEST=(\d+).*', '$1')
$r = & (Join-Path $PSScriptRoot 'joinguest.ps1') -Name 'Bravo' -GuestEnv @{COOP_DEBUG_BUILD = '109'}
$g2 = [int]($r -replace '.*GUEST=(\d+).*', '$1')
Start-Sleep $Seconds

function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }
$built = Get-Content "$d\coop_native_$h.log" | Select-String 'guest built object 109 at \((-?\d+), (-?\d+)\)' |
  Select-Object -Last 1
"HOST=$h ALPHA=$g1 BRAVO=$g2"
Check 'Bravo''s build reached the host' ($null -ne $built)
if ($built) {
  $x = $built.Matches[0].Groups[1].Value; $y = $built.Matches[0].Groups[2].Value
  $answer = & $query -GamePid $g1 "woken 109 at 109 $x $y"
  Check "Alpha has it at ($x, $y): $answer" ($answer -match '=> [1-9]')
}
Check 'all three games alive' (@(Get-Process -Id $h, $g1, $g2 -ErrorAction SilentlyContinue).Count -eq 3)
