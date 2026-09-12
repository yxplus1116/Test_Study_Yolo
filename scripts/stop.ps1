$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$sessionFile=Join-Path $projectRoot '.local/live-session.json'
if(!(Test-Path -LiteralPath $sessionFile)) { Write-Host 'No managed preview session.'; exit 0 }
$session=Get-Content -LiteralPath $sessionFile -Raw | ConvertFrom-Json
$process=Get-Process -Id $session.pid -ErrorAction SilentlyContinue
if(!$process) { Write-Host 'Preview has already exited.'; exit 0 }
$expected=Join-Path $projectRoot 'dist/TestStudyYolo.exe'
if($process.Path -ne $expected -or $process.StartTime.ToUniversalTime().Ticks -ne $session.startTicks) {
    throw 'Session PID no longer identifies this preview; no action taken.'
}
$stopFile=[IO.Path]::GetFullPath($session.stopFile)
$allowedRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot '.local/runlogs')).TrimEnd('\')+'\'
if(!$stopFile.StartsWith($allowedRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid session stop path' }
Set-Content -LiteralPath $stopFile -Value 'stop' -Encoding ASCII
if(!$process.WaitForExit(10000)) { throw 'Preview did not stop within 10 seconds; inspect its log or press ESC.' }
Write-Host 'Preview stopped cleanly.'
