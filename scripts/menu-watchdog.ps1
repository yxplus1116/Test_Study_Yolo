param([Parameter(Mandatory=$true)][string]$SessionPath)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$allowed = [IO.Path]::GetFullPath((Join-Path $projectRoot '.local/runlogs')).TrimEnd('\') + '\'
$resolved = [IO.Path]::GetFullPath($SessionPath)
if (!$resolved.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid session path' }
$session = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json
$expected = [IO.Path]::GetFullPath((Join-Path $projectRoot 'dist/TestStudyYolo.exe'))
$stopFile = [IO.Path]::GetFullPath([string]$session.stopFile)
if (!$stopFile.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid stop path' }
while ($true) {
    $child = Get-Process -Id $session.pid -ErrorAction SilentlyContinue
    if (!$child -or $child.Path -ne $expected -or $child.StartTime.ToUniversalTime().Ticks -ne $session.startTicks) { break }
    $owner = Get-Process -Id $session.ownerPid -ErrorAction SilentlyContinue
    if (!$owner -or $owner.StartTime.ToUniversalTime().Ticks -ne $session.ownerStartTicks) {
        Set-Content -LiteralPath $stopFile -Value 'menu-closed' -Encoding ASCII
        [void]$child.WaitForExit(10000)
        break
    }
    Start-Sleep -Milliseconds 500
}
