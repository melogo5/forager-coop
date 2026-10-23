# The game's sleep list (global.deactivatedInstances) stays clean while a guest fights: no dead ids (ProcessCulls
# stops the game with "Unable to find any instance" on one) and no doubles. Mutant twin: -Mutant wake (waking leaves
# ids on the lists) should FAIL.
# Both games run -Speed times as fast (COOP_DEBUG_SPEED): -Seconds of fighting are worth Speed x Seconds of play.
# Usage: sleeplists.ps1 [-Mutant wake] [-Seconds 25] [-Speed 4]
param([string]$Mutant = '', [int]$Seconds = 25, [int]$Speed = 4)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$extra = @{COOP_DEBUG_SPEED = "$Speed"}
if ($Mutant) { $extra.COOP_DEBUG_MUTANT = $Mutant }
$p = StartPair -HostEnv (@{COOP_DEBUG_ENEMY = '28'} + $extra) -GuestEnv (@{COOP_DEBUG_AUTOHIT = '1'} + $extra)
"HOST=$($p.Host) GUEST=$($p.Guest) mutant=$Mutant speed=$Speed"
Check "both games run x$Speed" ((WaitLog $p.Host "debug speed x$Speed set" 5) -and (WaitLog $p.Guest "debug speed x$Speed set" 5))
Start-Sleep $Seconds
foreach ($game in $p.Host, $p.Guest) {
  $list = Ask $game 'global 0x16c'
  $n = [int](Ask $game "call ds_list_size $list")
  $ids = @()
  if ($n -gt 0) {
    $reads = @(0..($n - 1) | ForEach-Object { "call ds_list_find_value $list $_" })
    $ids = @(& $Query -GamePid $game @reads | ForEach-Object { ($_ -split ' => ')[1] })
  }
  $vars = @($ids | ForEach-Object { "var $_ 0x4F" })
  $gone = if ($vars.Count -gt 0) { @(& $Query -GamePid $game @vars | Where-Object { $_ -match 'no such instance' }).Count } else { 0 }
  $doubles = $n - @($ids | Sort-Object -Unique).Count
  Check "game ${game}: the sleep list ($n) has no dead ids ($gone) and no doubles ($doubles)" ($gone -eq 0 -and $doubles -eq 0)
}
Check "no GML error in either game" (-not (Select-String -Path "$GameDir\coop_native_$($p.Host).log", "$GameDir\coop_native_$($p.Guest).log" -Pattern 'crash: the game reported' -Quiet))
Done
