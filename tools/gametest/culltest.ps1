# Runs one COOP_DEBUG_CULLTEST session and diffs the host's and the guest's object listings.
param([string]$Object = '1', [string]$GuestName = 'Builder', [hashtable]$GuestExtra = @{})
Set-Location z:\repos\forager-coop
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue; Start-Sleep 2
$r = & .\tools\gametest\coopflow.ps1 -GodMode -GuestName $GuestName -HostEnv @{COOP_DEBUG_CULLTEST = $Object} -GuestEnv (@{COOP_DEBUG_CULLTEST = $Object} + $GuestExtra)
$r
$h = [int]($r -replace '.*HOST=(\d+).*', '$1'); $g = [int]($r -replace '.*GUEST=(\d+).*', '$1')
for ($i = 0; $i -lt 60; $i++) {
  if ((Select-String "$d\coop_native_$g.log" -Pattern 'objects at the end' -Quiet) -and
      (Select-String "$d\coop_native_$h.log" -Pattern 'objects at the end' -Quiet)) { break }
  Start-Sleep 2
}
Start-Sleep 1
function Rocks($p, $when) {
  $lines = Get-Content "$d\coop_native_$p.log"
  $start = ($lines | Select-String "objects at the $when").LineNumber
  $out = @()
  for ($k = $start; $k -lt $lines.Count; $k++) {
    if ($lines[$k] -match 'object at \((\d+), (\d+)\)') { $out += "$($matches[1]),$($matches[2])" }
    elseif ($lines[$k] -match 'cull test') { break }
  }
  $out
}
foreach ($p in $h, $g) { "== $p"; Select-String "$d\coop_native_$p.log" -Pattern 'objects at the|destroyed|deactivated|failed' | % Line }
"misses: " + (Select-String "$d\coop_native_$g.log" -Pattern 'not found').Count
foreach ($w in 'start', 'end') {
  "-- $w diff (<= host only, => guest only)"
  Compare-Object @(Rocks $h $w) @(Rocks $g $w) | % { "$($_.SideIndicator) $($_.InputObject)" }
}
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
