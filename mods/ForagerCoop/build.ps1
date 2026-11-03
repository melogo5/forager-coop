# Builds the Steam Workshop package from native\build\version.dll (run native\build.ps1 first):
# mods\ForagerCoop\build\catch_error.dll (the bridge, MSVC x86) and ForagerCoop-Workshop-<version>.zip next to it.
# Usage: powershell -File mods\ForagerCoop\build.ps1 [-Version v0.1.3]
param([string]$Version = 'dev')
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x86 toolset not found (Visual Studio Build Tools with C++ workload).' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars32.bat'

$root = $PSScriptRoot
$out = Join-Path $root 'build'
$payload = Join-Path $root '..\..\native\build\version.dll'
if (-not (Test-Path $payload)) { throw "$payload not found: run native\build.ps1 first." }
New-Item -ItemType Directory -Force $out | Out-Null

$cl = "cl /nologo /O2 /MT /W3 /DNDEBUG /Fo`"$out\\`" `"$root\bridge.c`" " +
      "/LD /Fe`"$out\catch_error.dll`" /link /DEF:`"$root\bridge.def`" /SAFESEH:NO"
cmd /c "`"$vcvars`" >nul && $cl"
if ($LASTEXITCODE -ne 0) { throw "bridge build failed ($LASTEXITCODE)" }

python "$root\package.py" $payload "$out\catch_error.dll" $Version $out
if ($LASTEXITCODE -ne 0) { throw "packaging failed ($LASTEXITCODE)" }
