# Host + guest; the guest saves and exits to the main menu, then joins again through COOP -> JOIN.
# Prints the relevant log lines of both sides.
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
& $ctl -GamePid $g -Key Esc | Out-Null; Start-Sleep 1
Click $g '825,60'; Start-Sleep 1          # last tab (settings)
Click $g '930,505'                         # save and exit to menu
Start-Sleep 10
Click $g '675,640'; Start-Sleep 3          # COOP
for ($i = 0; $i -lt 10; $i++) {
  if ((Select-String "$d\coop_native_$g.log" -Pattern 'session: joining').Count -ge 2) { break }
  & $ctl -GamePid $g -Click '995,205' -HoldMs 150 | Out-Null; Start-Sleep 2   # JOIN
}
Start-Sleep 20
& $ctl -GamePid $g -Shot rejoin_guest | Out-Null
foreach ($p in $h, $g) {
  "== $p"
  Get-Content "$d\coop_native_$p.log" | Select-String -Pattern 'session:|net:|puppet:|world:|character: (own|new|guest|host)|menu: loading' | ForEach-Object Line
}
