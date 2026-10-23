# Playtest 5 #2: a vault the guest has open stays locked for the host while the guest moves items in it (the lock was
# released by the contents the guest sent while it was open - items doubled or got lost), and vaults have 9 cells for
# everyone once one player has skill 23 (COOP_DEBUG_SKILL=23 at the guest). Slot 5 must have a vault near the start.
# Mutant twins: -Mutant storagelock and -Mutant vaultslots must FAIL.
# Usage: vaults.ps1 [-Mutant storagelock|vaultslots]
param([string]$Mutant = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$extra = if ($Mutant) { @{COOP_DEBUG_MUTANT = $Mutant} } else { @{} }
$p = StartPair -HostEnv $extra -GuestEnv (@{COOP_DEBUG_SKILL = '23'} + $extra)
$h = $p.Host; $g = $p.Guest
"HOST=$h GUEST=$g mutant=$Mutant"
function Nearest([int]$game) {
  $xy = (Ask $game 'player') -split ' '
  $m = [regex]::Match((Ask $game "woken 114 list 114 $($xy[1]) $($xy[2]) 2000"), '^\d+: (\d+) ([\d.]+) ([\d.]+);')
  if (-not $m.Success) { return $null }
  return @{ id = $m.Groups[1].Value; x = $m.Groups[2].Value; y = $m.Groups[3].Value }
}
$hostVault = Nearest $h
Check 'the world has a vault' ($null -ne $hostVault)
if (-not $hostVault) { Done }
$guestVault = [regex]::Match((Ask $g "woken 114 list 114 $($hostVault.x) $($hostVault.y) 4"), '^\d+: (\d+)').Groups[1].Value
Check "the guest has the same vault at ($($hostVault.x), $($hostVault.y))" ([bool]$guestVault)

# Cells: the guest learns skill 23 ~10 s in; within a few seconds the host's vault has 9 cells too.
$cells = ''
for ($i = 0; $i -lt 30 -and $cells -ne '9'; $i++) { Start-Sleep 1; $cells = Ask $h "woken 114 var $($hostVault.id) 0x639" }
Check "the guest's skill gives the host's vault 9 cells too (has $cells)" ($cells -eq '9')

# Lock: the guest opens it and moves an item while it is open; the host is then turned away.
Ask $g "woken 114 event $guestVault 7 11" | Out-Null
Start-Sleep 2
$amount = Ask $g "var $guestVault 0x7C4 1"
Ask $g "set $guestVault 0x7C4 1 $([double]$(if ($amount -match '^[\d.]+$') { $amount } else { 0 }) + 1)" | Out-Null
Start-Sleep 3  # a watch of the vaults runs every second
$before = @(Select-String -Path "$GameDir\coop_native_$h.log" -Pattern 'is open by the other player').Count
Ask $h "woken 114 event $($hostVault.id) 7 11" | Out-Null
Start-Sleep 1
$after = @(Select-String -Path "$GameDir\coop_native_$h.log" -Pattern 'is open by the other player').Count
Check 'the host cannot open the vault the guest has open (and is changing)' ($after -gt $before)
Done
