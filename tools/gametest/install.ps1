# Stops running games and puts the freshly built version.dll next to Forager.exe (a game that is still closing
# keeps the file locked for a moment: retried).
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
$d = 'Z:\SteamLibrary\steamapps\common\Forager'
Stop-Process -Name Forager -Force -ErrorAction SilentlyContinue
Wait-Process -Name Forager -Timeout 30 -ErrorAction SilentlyContinue
for ($i = 0; ; $i++) {
  try { Copy-Item (Join-Path $root 'native\build\version.dll') "$d\version.dll" -Force; break }
  catch { if ($i -ge 10) { throw }; Start-Sleep 1 }
}
