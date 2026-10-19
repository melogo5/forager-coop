# One coop session that exercises most features at once and prints PASS/FAIL per check.
# Run it on a fresh world: delete the test saves first (never saveData0 - the user's real world).
# Usage: regress.ps1 [-Seconds 60]
param([int]$Seconds = 60)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
$errorLog = Join-Path $env:LOCALAPPDATA 'Forager\error.log'
$errorsBefore = if (Test-Path $errorLog) { (Get-Item $errorLog).LastWriteTime } else { $null }

Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -NewWorld -Cheats -GodMode -GuestName 'Regress' `
  -HostEnv @{COOP_DEBUG_ENEMY = '26'; COOP_DEBUG_COINS = '50'} `
  -GuestEnv @{COOP_DEBUG_AUTOHIT = '1'; COOP_DEBUG_BUILD = '109'; COOP_DEBUG_CRAFTDROP = '1'; COOP_DEBUG_COINS = '100'; COOP_DEBUG_STORAGE = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
# A chest next to the host (and the guest, who stands there too) before the guest's storage aid opens it (~7 s).
& $ctl -GamePid $h -Key T | Out-Null; Start-Sleep -Milliseconds 400
& $ctl -GamePid $h -Text '/spawn objVault' | Out-Null; & $ctl -GamePid $h -Key Enter | Out-Null
Start-Sleep $Seconds

$hostLog = Get-Content "$d\coop_native_$h.log"
$guestLog = Get-Content "$d\coop_native_$g.log"
function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }
function Has($log, [string]$pattern) { [bool]($log | Select-String -Pattern $pattern -Quiet) }
$alive = @(Get-Process -Id $h, $g -ErrorAction SilentlyContinue).Count -eq 2
$errorsAfter = if (Test-Path $errorLog) { (Get-Item $errorLog).LastWriteTime } else { $null }
$applied = @($hostLog | Select-String 'guest hit .*applied').Count
$missed = @($hostLog | Select-String 'guest hit .*not found').Count

"HOST=$h GUEST=$g"
Check 'both games alive' $alive
Check 'no GML errors (error.log unchanged)' ($errorsAfter -eq $errorsBefore)
Check "guest hits applied on the host ($applied applied, $missed not found)" ($applied -gt 0 -and $missed -eq 0)
Check 'guest build reached the host' (Has $hostLog 'guest built object 109')
Check 'shared chest: guest put items in, host got them' (Has $hostLog 'storage: chest .* updated from the peer')
Check 'crafted drop went into the guest inventory' (Has $guestLog 'crafted items straight into the inventory')
Check 'guest coins reached the host' (Has $hostLog 'coins: the guest earned 100')
Check 'guest shows the shared progress' (Has $guestLog 'shared progress: level')
Check 'time of day reached the guest' (Has $guestLog 'clock: light state')
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
