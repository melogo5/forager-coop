# The guest's loot (PLAYTEST8.md #14, #15): the guest's owned gear reaches its puppet at the host (amulets count
# in the drops the host makes for the guest), and drops reach the guest with their stack (DropItem merges drops:
# lighthouses and amulets show as stack > 1). The guest auto-hits trees (COOP_DEBUG_AUTOHIT=85).
# Usage: guestloot.ps1 [-Slot 5]
param([int]$Slot = 5)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
& (Join-Path $PSScriptRoot 'install.ps1')
$p = StartPair -GuestEnv @{COOP_DEBUG_AUTOHIT = '85'} -Slot $Slot

# 1. Amulet: the guest owns the golden amulet (gear category 7 = item 131); its puppet at the host gets it.
$guestPlayer = (Ask $p.Guest 'player').Split(' ')[0]
Ask $p.Guest "set $guestPlayer 0x535 7 131" | Out-Null
Start-Sleep 3
$players = (Ask $p.Host 'list 22') -split ';' | ForEach-Object { (($_.Trim() -replace '^\d+: ', '') -split ' ')[0] }
$puppetGear = $players | ForEach-Object { Ask $p.Host "var $_ 0x535 7" }
Check "the guest's amulet is on its puppet at the host (gear 7 per objPlayer: $($puppetGear -join ', '))" `
  ($puppetGear -contains '131')

# 2. Lighthouses: two at the host next to the guest double the drops of the trees the guest fells (about 3 -> 7).
$at = (Ask $p.Guest 'player').Split(' ')
foreach ($dx in -20, 20) { Ask $p.Host "call instance_create_depth $([double]$at[1] + $dx) $($at[2]) -500 129" | Out-Null }
Start-Sleep 50
$bursts = Select-String "$GameDir\coop_native_$($p.Guest).log" -Pattern 'our drop of item' |
  Group-Object { $_.Line.Substring(0, 9) } | ForEach-Object { $_.Count }  # drops of one tree land within 0.1 s
"  drops per felled tree at the guest: $($bursts -join ', ')"
Check 'lighthouses raise the drops of a guest-felled tree (6+)' (($bursts | Measure-Object -Maximum).Maximum -ge 6)
Done
