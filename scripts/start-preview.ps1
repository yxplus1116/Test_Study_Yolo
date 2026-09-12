$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$sessionFile=Join-Path $projectRoot '.local/live-session.json'
$exe=Join-Path $projectRoot 'dist/TestStudyYolo.exe'
if(!(Test-Path -LiteralPath $exe)) { throw 'Build the project first.' }
if(Test-Path -LiteralPath $sessionFile) {
    $previous=Get-Content -LiteralPath $sessionFile -Raw | ConvertFrom-Json
    $existing=Get-Process -Id $previous.pid -ErrorAction SilentlyContinue
    if($existing -and $existing.Path -eq $exe -and $existing.StartTime.ToUniversalTime().Ticks -eq $previous.startTicks) {
        Write-Host "Preview is already running (PID $($existing.Id))."; exit 0
    }
}
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$output=Join-Path $projectRoot ".local/runlogs/live-$stamp"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$stopFile=Join-Path $output 'stop.request'
$localDeps=Join-Path $projectRoot '.local'
$env:PATH="$localDeps\tensorrt\tensorrt_libs;$localDeps\cuda\bin;$localDeps\opencv-dist\opencv\build\x64\vc16\bin;$env:PATH"
$arguments='--capture gdi --output "{0}" --stop-file "{1}"' -f $output,$stopFile
$process=Start-Process -FilePath $exe -WorkingDirectory $projectRoot -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$output/stdout.log" -RedirectStandardError "$output/stderr.log"
$startTicks=$process.StartTime.ToUniversalTime().Ticks
@{pid=$process.Id;startTicks=$startTicks;stopFile=$stopFile;output=$output;executable=$exe} |
    ConvertTo-Json | Set-Content -LiteralPath $sessionFile -Encoding UTF8
Start-Sleep -Milliseconds 800
$process.Refresh()
if($process.HasExited) { throw "Preview exited ($($process.ExitCode)). Read $output/stderr.log." }
Write-Host "Detection preview started (PID $($process.Id)); mouse output disabled. ESC or scripts/stop.ps1 exits."
