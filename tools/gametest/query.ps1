# Asks a running game (started with COOP_DEBUG_PORT=1) about its state: one answer line per command.
# Commands: see native/src/debugport.h (ping, player, count, list, at, var, global, call).
# Usage: query.ps1 -GamePid 1234 'player' 'count 109' 'at 109 672 640'
param(
  [Parameter(Mandatory)][int]$GamePid,
  [Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Commands
)
$ErrorActionPreference = 'Stop'
$log = "Z:\SteamLibrary\steamapps\common\Forager\coop_native_$GamePid.log"
$line = Select-String -Path $log -Pattern 'debugport: listening on 127\.0\.0\.1:(\d+)' | Select-Object -Last 1
if (-not $line) { throw "game ${GamePid}: no debug port in its log (start it with COOP_DEBUG_PORT=1)" }
$port = [int]$line.Matches[0].Groups[1].Value
$client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $port)
try {
  $stream = $client.GetStream()
  $writer = New-Object System.IO.StreamWriter($stream); $writer.AutoFlush = $true
  $reader = New-Object System.IO.StreamReader($stream)
  foreach ($command in $Commands) {
    $writer.WriteLine($command)
    '{0} => {1}' -f $command, $reader.ReadLine()
  }
} finally {
  $client.Close()
}
