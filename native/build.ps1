# Builds native\build\version.dll (x86, static CRT) with the MSVC x86 toolset.
# Usage: powershell -File native\build.ps1 [-Install]   (-Install copies it next to Forager.exe)
param(
    [switch]$Install,
    [string]$GameDir = 'Z:\SteamLibrary\steamapps\common\Forager'
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x86 toolset not found (Visual Studio Build Tools with C++ workload).' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars32.bat'

$root = $PSScriptRoot
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$mh = Join-Path $root 'third_party\minhook'
$sources = @(
    "$root\src\dllmain.cpp", "$root\src\yyc.cpp", "$root\src\log.cpp", "$root\src\trace.cpp", "$root\src\crash.cpp",
    "$root\src\net.cpp", "$root\src\session.cpp", "$root\src\puppet.cpp", "$root\src\world.cpp", "$root\src\sync.cpp", "$root\src\sync_debug.cpp", "$root\src\sync_hits.cpp", "$root\src\sync_progress.cpp", "$root\src\sync_wake.cpp", "$root\src\mobs.cpp", "$root\src\character.cpp", "$root\src\storage.cpp", "$root\src\land.cpp", "$root\src\clock.cpp", "$root\src\unlocks.cpp", "$root\src\coins.cpp", "$root\src\progress.cpp", "$root\src\quests.cpp", "$root\src\stations.cpp", "$root\src\skills.cpp", "$root\src\reconcile.cpp", "$root\src\chests.cpp", "$root\src\steam.cpp", "$root\src\skilltab.cpp", "$root\src\fishing.cpp", "$root\src\debugport.cpp", "$root\src\look.cpp", "$root\src\traps.cpp", "$root\src\banks.cpp", "$root\src\interiors.cpp",
    "$root\src\gui.cpp", "$root\src\lan.cpp", "$root\src\menu.cpp", "$root\src\savefix.cpp", "$root\src\files.cpp", "$root\src\familiars.cpp", "$root\src\bombs.cpp", "$root\src\dungeons.cpp", "$root\src\puzzles.cpp", "$root\src\intro.cpp",
    "$mh\src\hook.c", "$mh\src\buffer.c", "$mh\src\trampoline.c", "$mh\src\hde\hde32.c"
) | ForEach-Object { "`"$_`"" }

$cl = "cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /DWIN32_LEAN_AND_MEAN /DNDEBUG " +
      "/I`"$mh\include`" /Fo`"$out\\`" " + ($sources -join ' ') +
      " /LD /Fe`"$out\version.dll`" /link /DEF:`"$root\src\version.def`" /SAFESEH:NO"
cmd /c "`"$vcvars`" >nul && $cl"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built $out\version.dll"

if ($Install) {
    Copy-Item "$out\version.dll" (Join-Path $GameDir 'version.dll') -Force
    New-Item -ItemType File -Force (Join-Path $GameDir 'coop_skip_intro.txt') | Out-Null  # dev: no startup logos
    Write-Host "installed to $GameDir (remove version.dll there to uninstall)"
}
