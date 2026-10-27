# The host goes back to the menu: the guest saves at once and its last save reaches the host before the session ends
# (before, up to 15 s of the guest's play were lost).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
& (Join-Path $PSScriptRoot 'install.ps1')
$p = StartPair -Slot 5
$h = $p.Host; $g = $p.Guest
Start-Sleep 5
Ask $h 'script 0x4A5620' | Out-Null  # RestartGame: what "save and exit" and game over use to reach the menu
Check 'the host waits for the guest' (WaitLog $h 'waiting for the last save of 1 guest' 15)
Check 'the guest saves for it' (WaitLog $g 'the host is leaving - saving the character' 15)
Check 'the host stores that save' (WaitLog $h 'stopped hosting' 15)
$log = Get-Content "$GameDir\coop_native_$h.log"
$wait = ($log | Select-String 'waiting for the last save').LineNumber | Select-Object -Last 1
$saved = ($log | Select-String "saved guest 'Regress'").LineNumber | Select-Object -Last 1
$stopped = ($log | Select-String 'stopped hosting').LineNumber | Select-Object -Last 1
Check "the save came after the goodbye and before the end ($wait < $saved < $stopped)" (($wait -lt $saved) -and ($saved -lt $stopped))
Check 'the guest is back in the menu' (WaitLog $g 'the host left; back to the main menu' 15)
$log | Select-String 'session:|character:' | Select-Object -Last 6 | ForEach-Object Line
'--- guest'
Get-Content "$GameDir\coop_native_$g.log" | Select-String 'session:|character:' | Select-Object -Last 6 | ForEach-Object Line
Done
