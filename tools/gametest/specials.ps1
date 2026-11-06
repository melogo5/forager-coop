# PLAYTEST10: the museum (rainbow, 4 pillars...) of a bought island did not appear for anyone. The guest replays
# SpawnLand with the host's seed, and that replay clears the spot of the island's special; the guest reported the
# destroy and the host took its own copy away. The host buys grassland islands (the sale signs are made grassland),
# each with one template left in the grassland pool, so each brings a known special: 8 museum, 2 four pillars,
# 5 rainbow, 7 fairy pond, 6 objects 167/168. Both must have every one of them.
# Run on a test slot (it buys islands there). Usage: specials.ps1 [-HostSlot 5]
param([int]$HostSlot = 5)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
& (Join-Path $PSScriptRoot 'install.ps1')
$pair = StartPair -Slot $HostSlot
$h = $pair.Host; $g = $pair.Guest
# Sale signs exist only while the BUY LAND tab of the player menu is open.
$ctl = Join-Path $PSScriptRoot 'gamectl.ps1'
& $ctl -GamePid $h -Key Esc | Out-Null; Start-Sleep 1
& $ctl -GamePid $h -Click '735,60' -HoldMs 300 | Out-Null; Start-Sleep -Milliseconds 400
& $ctl -GamePid $h -Click '735,60' -HoldMs 150 | Out-Null; Start-Sleep 2
$kBiomeSlot = '0xB4'  # objPurchaseLand.biome
$signs = [regex]::Matches((Ask $h 'woken 229 list 229'), '(\d+) [\d.]+ [\d.]+;')
Check "enough islands for sale ($($signs.Count))" ($signs.Count -ge 5)
foreach ($m in $signs) { Ask $h "woken 229 set $($m.Groups[1].Value) $kBiomeSlot 0" | Out-Null }
# global.landPool: a ds_list of templates per biome; the first is grassland's.
$grassPool = [int][regex]::Match((Ask $h 'globalarr 0x5eb'), '^\[(\d+)').Groups[1].Value
foreach ($template in 8, 2, 5, 7, 6) {
  $n = [int](Ask $h "call ds_list_size $grassPool")
  for ($i = 0; $i -lt $n; $i++) { Ask $h "call ds_list_delete $grassPool 0" | Out-Null }
  Ask $h "call ds_list_add $grassPool $template" | Out-Null
  Ask $h 'woken 229 buyland' | Out-Null; Start-Sleep 3
}
Start-Sleep 5
function Count([int]$game, [int]$object) { [regex]::Matches((Ask $game 'dump'), "(?<=[:;] )$object [\d.-]+ [\d.-]+").Count }
foreach ($o in @(@(171, 'museum', 1), @(164, 'pillars', 4), @(169, 'rainbow', 1), @(170, 'fairy pond', 1))) {
  $atHost = Count $h $o[0]; $atGuest = Count $g $o[0]
  Check "$($o[1]) ($($o[0])): host $atHost, guest $atGuest (want $($o[2]) at both)" ($atHost -eq $o[2] -and $atGuest -eq $o[2])
}
$hostCount = (Ask $h 'dump') -replace ':.*', ''; $guestCount = (Ask $g 'dump') -replace ':.*', ''
Check "as many world objects at the guest ($guestCount) as at the host ($hostCount)" ($hostCount -eq $guestCount)
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Done
