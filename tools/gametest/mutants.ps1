# Mutation twins: every check runs as is (must PASS) and with its fix taken out (COOP_DEBUG_MUTANT, must FAIL).
# A check that passes with its mutant proves nothing. Usage: mutants.ps1 [-Only chests,skills]
param([string[]]$Only = @())
$ErrorActionPreference = 'Continue'
$checks = [ordered]@{
  chests     = @{ Script = 'chests.ps1';      Mutant = 'chests' }
  skills     = @{ Script = 'guestskills.ps1'; Mutant = 'skills' }
  savelists  = @{ Script = 'savelists.ps1';   Mutant = 'savelists' }
  herbs      = @{ Script = 'herbs.ps1';       Mutant = 'herbs' }
  wake       = @{ Script = 'sleeplists.ps1';  Mutant = 'wake' }
  modes      = @{ Script = 'modes.ps1';       Mutant = 'modes' }
  worlddiff  = @{ Script = 'worlddiff-selfcheck.ps1'; Mutant = '' }
}
$rows = @()
foreach ($name in $checks.Keys) {
  if ($Only.Count -gt 0 -and $Only -notcontains $name) { continue }
  $c = $checks[$name]
  $script = Join-Path $PSScriptRoot $c.Script
  "== $name"
  & $script | ForEach-Object { "   $_" }
  $plain = $LASTEXITCODE
  $twin = $null
  if ($c.Mutant) {
    "== $name with mutant $($c.Mutant)"
    & $script -Mutant $c.Mutant | ForEach-Object { "   $_" }
    $twin = $LASTEXITCODE
  }
  $rows += [pscustomobject]@{
    Check = $name
    Plain = $(if ($plain -eq 0) { 'PASS' } else { 'FAIL' })
    Mutant = $(if ($null -eq $twin) { '-' } elseif ($twin -ne 0) { 'FAILS (good)' } else { 'PASSES (test is blind)' })
  }
}
Get-Process Forager -ErrorAction SilentlyContinue | Stop-Process -Force
$rows | Format-Table -AutoSize
