# PLAYTEST2 #8: every player wears its own costume, and the others' puppets show it. The guest's own costume is
# written to its look file before the start; the host changes its costume during the session.
# Uses the most recent save (row 1). Usage: costumes.ps1
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path (Split-Path $PSScriptRoot))
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
$query = Join-Path $PSScriptRoot 'query.ps1'
$saves = Join-Path $env:LOCALAPPDATA 'Forager'
$guestLook = '0 64 83 94'
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
& (Join-Path $PSScriptRoot 'install.ps1')
Set-Content -Path (Join-Path $saves 'coop_look_guest.txt') -Value $guestLook -Encoding ascii
$r = & (Join-Path $PSScriptRoot 'coopflow.ps1') -HostSlotRow 1 -GodMode `
  -HostEnv @{COOP_DEBUG_PORT = '1'; COOP_DEBUG_LOOK_FILE = 'coop_look_host.txt'} `
  -GuestEnv @{COOP_DEBUG_PORT = '1'; COOP_DEBUG_LOOK_FILE = 'coop_look_guest.txt'}
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
function Ask([int]$id, [string]$command) { ((& $query -GamePid $id $command) -replace '^.*? => ', '') }
function Check([string]$name, [bool]$ok) { '{0,-4} {1}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name }
# The costume of an objPlayer: its 4 cosmetic slots.
function LookOf([int]$game, [int]$player) { ((0..3) | ForEach-Object { Ask $game "var $player 0x135 $_" }) -join ' ' }
# A game's own player and the puppet of the other one.
function Players([int]$game) {
  $me = ((Ask $game 'player') -split ' ')[0]
  $all = [regex]::Matches((Ask $game 'list 22'), '(\d+) [\d.]+ [\d.]+;') | ForEach-Object { $_.Groups[1].Value }
  return @{ me = $me; puppet = ($all | Where-Object { $_ -ne $me } | Select-Object -First 1) }
}
Start-Sleep 4
$hp = Players $h; $gp = Players $g
$hostLook = LookOf $h $hp.me
"HOST=$h GUEST=$g; host wears $hostLook"
Check "the guest wears its own costume ($(LookOf $g $gp.me))" ((LookOf $g $gp.me) -eq $guestLook)
Check "the host's puppet of the guest wears it ($(LookOf $h $hp.puppet))" ((LookOf $h $hp.puppet) -eq $guestLook)
Check "the guest's puppet of the host wears the host's ($(LookOf $g $gp.puppet))" ((LookOf $g $gp.puppet) -eq $hostLook)

$hat = [int]((Ask $h "var $($hp.me) 0x135 1")) + 1
Ask $h "set $($hp.me) 0x135 1 $hat" | Out-Null
Start-Sleep 3
Check "the host changed its costume, the guest's puppet of it too ($(LookOf $g $gp.puppet))" (((LookOf $g $gp.puppet) -split ' ')[1] -eq "$hat")
Ask $h "set $($hp.me) 0x135 1 $($hat - 1)" | Out-Null  # as it was
Start-Sleep 2
Check "the guest's look file is still its own" ((Get-Content (Join-Path $saves 'coop_look_guest.txt')) -eq $guestLook)
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
