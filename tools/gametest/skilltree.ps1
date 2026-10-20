# The skill tree over the running world: the host (or with -OnGuest the guest) opens it, holds the mouse on the
# first available skill until it is bought, then leaves with Esc. Uses the most recently played save (row 1);
# -Shot saves shots	ree_hover.png and shots	ree_closed.png. Look for 'learned 1' and 'the skill tree closed'.
# Usage: skilltree.ps1 [-OnGuest] [-Shot] [-HoldMs 5000]
param([string]$At = '640,360', [int]$HoldMs = 5000, [switch]$Shot, [hashtable]$ExtraEnv = @{}, [switch]$OnGuest)
$ErrorActionPreference = 'Stop'
Set-Location 'Z:\repos\forager-coop'
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
$ctl = 'tools\gametest\gamectl.ps1'
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
& 'tools\gametest\install.ps1'
$treeEnv = @{COOP_DEBUG_SKILLTREE = '45'} + $ExtraEnv
$hostEnv = @{COOP_DEBUG_XP = '400'} + $(if ($OnGuest) { @{} } else { $treeEnv })
$guestEnv = $(if ($OnGuest) { $treeEnv } else { @{} })
$r = & 'tools\gametest\coopflow.ps1' -HostSlotRow 1 -GodMode -HostEnv $hostEnv -GuestEnv $guestEnv
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
$hostPid = $h; $guestPid = $g
if ($OnGuest) { $h = $guestPid }  # the tree's side: its window and log
$log = "$d\coop_native_$h.log"
for ($i = 0; $i -lt 90 -and -not (Select-String -Path $log -Pattern 'skill tree opened' -Quiet); $i++) { Start-Sleep 1 }
Start-Sleep 3

$avail = Get-Content $log | Select-String 'skilltab: skill at \((\d+), (\d+)\) available' | Select-Object -First 1
if ($avail) {  # tree pixels -> screen through the tree camera (centred in x; in y it follows the mouse's height)
  $z = 720 / 277.714; $tx = [double]$avail.Matches[0].Groups[1].Value; $ty = [double]$avail.Matches[0].Groups[2].Value
  $sy = 360.0
  for ($k = 0; $k -lt 30; $k++) { $cy = 360 + ($sy / 360 - 1) * 110; $sy = ($ty - ($cy - 277.714 / 2)) * $z }
  $At = '{0},{1}' -f [int](($tx - (640 - 493.714 / 2)) * $z), [int]$sy
}
"hold at $At"
if ($Shot) {
  & $ctl -GamePid $h -Click $At -HoldMs 300 | Out-Null
  Start-Sleep 1
  & $ctl -GamePid $h -Shot 'tree_hover' | Out-Null
}
& $ctl -GamePid $h -Click $At -HoldMs $HoldMs | Out-Null
Start-Sleep 2
& $ctl -GamePid $h -Key Esc | Out-Null
Start-Sleep 2
& $ctl -GamePid $h -Shot 'tree_closed' | Out-Null
Start-Sleep 3
"HOST=$hostPid GUEST=$guestPid"
Get-Content $log | Select-String 'skilltab|points to spend|error'
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
