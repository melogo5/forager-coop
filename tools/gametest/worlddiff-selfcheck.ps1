# world-diff.ps1 must be able to fail: the worlds match, then an object made at the guest only shows up as a
# difference, and after it is gone they match again.
# Usage: worlddiff-selfcheck.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$p = StartPair
$h = $p.Host; $g = $p.Guest
"HOST=$h GUEST=$g"
$diff = Join-Path $PSScriptRoot 'world-diff.ps1'
& $diff -HostPid $h -GuestPid $g -Settle 3 | Out-Null
Check 'the worlds match after joining' ($LASTEXITCODE -eq 0)
$xy = (Ask $g 'player') -split ' '
$rock = Ask $g "call instance_create_depth $([double]$xy[1] + 48) $($xy[2]) 0 84"  # made here only, never sent
$out = & $diff -HostPid $h -GuestPid $g
Check "a rock made at the guest only is a difference ($(($out | Select-Object -First 1)))" ($LASTEXITCODE -eq 1 -and ($out -match 'guest only: object 84'))
Ask $g "call instance_destroy $rock 0" | Out-Null
& $diff -HostPid $h -GuestPid $g -Settle 2 | Out-Null
Check 'the worlds match again once it is gone' ($LASTEXITCODE -eq 0)
Done
