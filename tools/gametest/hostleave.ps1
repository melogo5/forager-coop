# Host + guest; the host saves and exits to the menu (the guest should follow), then the host creates again
# and the guest joins again. Prints the relevant log lines of both sides.
Set-Location z:\repos\forager-coop
$ctl = '.\tools\gametest\gamectl.ps1'
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
function Click([int]$id, [string]$at) {
  & $ctl -GamePid $id -Click $at -HoldMs 400 | Out-Null; Start-Sleep -Milliseconds 400
  & $ctl -GamePid $id -Click $at -HoldMs 150 | Out-Null
}
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
$r = & .\tools\gametest\coopflow.ps1 -GodMode -GuestName 'Builder'
$r
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
Start-Sleep 5
& $ctl -GamePid $h -Key Esc | Out-Null; Start-Sleep 1
Click $h '825,60'; Start-Sleep 1
Click $h '930,505'
Start-Sleep 12
& $ctl -GamePid $g -Shot hostleave_guest | Out-Null
# Again: the host creates, the guest joins.
Click $h '675,640'; Start-Sleep 2
& $ctl -GamePid $h -Click '993,114' -HoldMs 150 | Out-Null; Start-Sleep 3     # CREATE
Click $h '1000,138'                                                           # top save slot
Start-Sleep 15
Click $g '675,640'; Start-Sleep 3
for ($i = 0; $i -lt 15; $i++) {
  if ((Select-String "$d\coop_native_$g.log" -Pattern 'session: joining').Count -ge 2) { break }
  & $ctl -GamePid $g -Click '995,205' -HoldMs 150 | Out-Null; Start-Sleep 2
}
Start-Sleep 20
foreach ($p in $h, $g) {
  "== $p"
  Get-Content "$d\coop_native_$p.log" | Select-String -Pattern 'session:|net:|puppet:|world:|menu: loading' | ForEach-Object Line
}
