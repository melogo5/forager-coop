# Four players: the host and three guests on a new world. Guests 1 and 2 hit rocks (each gets only its own
# drops), guest 3 fills water and donates to the museum (both reach the other guests through the host).
# Prints PASS/FAIL per check. Run on test saves only (it deletes every saveData*).
param([int]$Seconds = 40)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
$errorLog = Join-Path $env:LOCALAPPDATA 'Forager\error.log'
$errorsBefore = if (Test-Path $errorLog) { (Get-Item $errorLog).LastWriteTime } else { $null }

Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
Get-ChildItem "$env:LOCALAPPDATA\Forager" -Directory -Filter 'saveData*' | Remove-Item -Recurse -Force
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -NewWorld -Cheats -GuestName 'Alpha' -GuestEnv @{COOP_DEBUG_AUTOHIT = '84'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g1 = [int]($r -replace '.*GUEST=(\d+).*', '$1')
$r = & (Join-Path $PSScriptRoot 'joinguest.ps1') -Name 'Bravo' -GuestEnv @{COOP_DEBUG_AUTOHIT = '84'}
$g2 = [int]($r -replace '.*GUEST=(\d+).*', '$1')
$r = & (Join-Path $PSScriptRoot 'joinguest.ps1') -Name 'Charlie' -GuestEnv @{COOP_DEBUG_LANDFILL = '1'; COOP_DEBUG_MUSEUM = '5'}
$g3 = [int]($r -replace '.*GUEST=(\d+).*', '$1')
Start-Sleep $Seconds
& $ctl -GamePid $h -Shot four_host | Out-Null

function Log([int]$id) { Get-Content "$d\coop_native_$id.log" }
function Has([int]$id, [string]$pattern) { [bool](Log $id | Select-String -Pattern $pattern -Quiet) }
function Count([int]$id, [string]$pattern) { @(Log $id | Select-String -Pattern $pattern).Count }
function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }
function SeesAll([int]$id, [int[]]$players) { @($players | Where-Object { -not (Has $id "created for player $_$") }).Count -eq 0 }

"HOST=$h GUEST1=$g1 GUEST2=$g2 GUEST3=$g3"
Check 'all four games alive' (@(Get-Process -Id $h, $g1, $g2, $g3 -ErrorAction SilentlyContinue).Count -eq 4)
Check 'three guests connected' ((Has $h 'guest 1 connected') -and (Has $h 'guest 2 connected') -and (Has $h 'guest 3 connected'))
Check 'host sees guests 1-3' (SeesAll $h 1, 2, 3)
Check 'guest 1 sees host, guests 2, 3' (SeesAll $g1 0, 2, 3)
Check 'guest 2 sees host, guests 1, 3' (SeesAll $g2 0, 1, 3)
Check 'guest 3 sees host, guests 1, 2' (SeesAll $g3 0, 1, 2)
$to1 = Count $h 'drop of item .* goes to guest 1'; $to2 = Count $h 'drop of item .* goes to guest 2'
$got1 = Count $g1 'our drop of item .* collected'; $got2 = Count $g2 'our drop of item .* collected'
Check "own drops only (guest 1: $got1 of $to1, guest 2: $got2 of $to2, guest 3: $(Count $g3 'our drop'))" `
  ($to1 -gt 0 -and $to2 -gt 0 -and $got1 -le $to1 -and $got2 -le $to2 -and $got1 -gt 0 -and $got2 -gt 0 -and (Count $g3 'our drop') -eq 0)
Check 'guest 3 landfill reached guests 1 and 2' ((Has $g1 "land: the peer's landfill at") -and (Has $g2 "land: the peer's landfill at"))
Check 'guest 3 museum donation reached guests 1 and 2' ((Has $g1 'progress: .* updated from the peer') -and (Has $g2 'progress: .* updated from the peer'))
Check 'no GML errors (error.log unchanged)' ($(if (Test-Path $errorLog) { (Get-Item $errorLog).LastWriteTime } else { $null }) -eq $errorsBefore)
