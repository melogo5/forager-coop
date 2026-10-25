# Two donations to the museum within the same second both stay (REVIEW.md: each side replaced the whole list with
# the other's, so one was lost). The host and the guest each add a different item to global.museumBundles back
# to back through the query port; a few seconds later both lists must hold both. Run on a test slot (the two
# donations stay in its museum). Prints PASS/FAIL per check. Usage: museum.ps1 [-HostSlot 5]
param([int]$HostSlot = 5)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
. (Join-Path $PSScriptRoot 'lib.ps1')

Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlot $HostSlot -HostEnv @{COOP_DEBUG_PORT = '1'} `
  -GuestEnv @{COOP_DEBUG_PORT = '1'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
Start-Sleep 5

function Museum([int]$game) {
  $id = Ask $game 'global 0x68A'
  $size = [int](Ask $game "call ds_list_size $id")
  @(for ($i = 0; $i -lt $size; $i++) { [double](Ask $game "call ds_list_find_value $id $i") })
}
$before = Museum $h
$items = @(1..60 | Where-Object { $before -notcontains [double]$_ } | Select-Object -First 2)
Ask $h "call ds_list_add $(Ask $h 'global 0x68A') $($items[0])" | Out-Null
Ask $g "call ds_list_add $(Ask $g 'global 0x68A') $($items[1])" | Out-Null
Start-Sleep 5
$atHost = Museum $h; $atGuest = Museum $g
"HOST=$h GUEST=$g; donated $($items[0]) at the host and $($items[1]) at the guest"
Check "the host's museum has both ($($atHost.Count) entries)" ($atHost -contains [double]$items[0] -and $atHost -contains [double]$items[1])
Check "the guest's museum has both ($($atGuest.Count) entries)" ($atGuest -contains [double]$items[0] -and $atGuest -contains [double]$items[1])
Check 'the two lists hold the same entries' ((@($atHost | Sort-Object) -join ',') -eq (@($atGuest | Sort-Object) -join ','))
function Sends { @(Get-Content "$GameDir\coop_native_$h.log", "$GameDir\coop_native_$g.log" | Select-String 'museum changed here').Count }
$sends = Sends; Start-Sleep 5
Check 'then they stop sending it (no back and forth)' ((Sends) -eq $sends)
