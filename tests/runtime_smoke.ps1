param([switch]$SkipCapture)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$localDeps=Join-Path $projectRoot '.local'
$exe=Join-Path $projectRoot 'dist/TestStudyYolo.exe'
$env:PATH="$localDeps\tensorrt\tensorrt_libs;$localDeps\cuda\bin;$localDeps\opencv-dist\opencv\build\x64\vc16\bin;$env:PATH"
$testRoot=Join-Path $localDeps ('runtime-smoke/'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$results=[Collections.Generic.List[object]]::new()
function Case([string]$Name,[string]$Arguments,[int]$Expected=0) {
    $stdout=Join-Path $testRoot "$Name.stdout.log"
    $stderr=Join-Path $testRoot "$Name.stderr.log"
    $p=Start-Process -FilePath $exe -WorkingDirectory $projectRoot -ArgumentList $Arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if(!$p.WaitForExit(15000)) { throw "$Name exceeded its test deadline. PID $($p.Id)" }
    $exitCode=$p.ExitCode
    $results.Add(@{name=$Name;exitCode=$exitCode;expected=$Expected;passed=($exitCode -eq $Expected)})
    if($exitCode -ne $Expected) { throw "$Name exit $exitCode, expected $Expected. See $stderr" }
}
Case 'missing-model' '--model .local/does-not-exist.onnx --headless --seconds 1' 1
Case 'invalid-monitor' '--monitor 999 --headless --seconds 1' 1
Case 'offline-input-rejected' '--image workspace/inference/gril.jpg --enable-input --headless' 1
$imageOut=Join-Path $testRoot 'image'
Case 'image-dry-run' ('--image workspace/inference/gril.jpg --label 1 --headless --frames 20 --output "{0}"' -f $imageOut)
$movements=Import-Csv -LiteralPath "$imageOut/movement.csv"
if(!$movements -or ($movements | Where-Object injected -ne '0')) { throw 'Dry run produced no control samples or injected input.' }
if(!$SkipCapture) {
    Case 'dxgi' ('--capture dxgi --headless --seconds 3 --output "{0}"' -f (Join-Path $testRoot 'dxgi'))
    Case 'gdi' ('--capture gdi --headless --seconds 3 --output "{0}"' -f (Join-Path $testRoot 'gdi'))
}
$stopFile=Join-Path $testRoot 'stop.request'
$stopOut=Join-Path $testRoot 'shutdown'
$arguments='--headless --stop-file "{0}" --output "{1}"' -f $stopFile,$stopOut
$stdout=Join-Path $testRoot 'shutdown.stdout.log'
$p=Start-Process -FilePath $exe -WorkingDirectory $projectRoot -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $testRoot 'shutdown.stderr.log')
$deadline=(Get-Date).AddSeconds(10)
do {
    Start-Sleep -Milliseconds 100
    $p.Refresh()
    if($p.HasExited) { throw "Shutdown test exited early: $($p.ExitCode)" }
    $started=(Get-Content -LiteralPath $stdout -Raw) -match 'Started:'
} while(!$started -and (Get-Date) -lt $deadline)
if(!$started) { throw 'Application failed to start during shutdown test' }
$timer=[Diagnostics.Stopwatch]::StartNew()
Set-Content -LiteralPath $stopFile -Value 'stop'
if(!$p.WaitForExit(3000) -or $p.ExitCode -ne 0) { throw 'Graceful shutdown failed' }
$timer.Stop()
$results.Add(@{name='graceful-shutdown';passed=$true;milliseconds=$timer.ElapsedMilliseconds;exitCode=$p.ExitCode})
$buildStop=Join-Path $testRoot 'build-stop.request'
$buildStdout=Join-Path $testRoot 'build-cancel.stdout.log'
$arguments='--rebuild --headless --seconds 1 --stop-file "{0}" --output "{1}"' -f $buildStop,(Join-Path $testRoot 'build-cancel')
$p=Start-Process -FilePath $exe -WorkingDirectory $projectRoot -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $buildStdout -RedirectStandardError (Join-Path $testRoot 'build-cancel.stderr.log')
$deadline=(Get-Date).AddSeconds(10)
do {
    Start-Sleep -Milliseconds 100
    $p.Refresh()
    if($p.HasExited) { throw 'Engine-build cancellation test exited early' }
    $building=(Get-Content -LiteralPath $buildStdout -Raw) -match 'Building TensorRT engine'
} while(!$building -and (Get-Date) -lt $deadline)
if(!$building) { throw 'Engine build failed to start' }
Set-Content -LiteralPath $buildStop -Value 'stop'
if(!$p.WaitForExit(15000) -or $p.ExitCode -ne 0) { throw 'Engine build cancellation failed' }
if((Get-Content -LiteralPath $buildStdout -Raw) -notmatch 'GPU operation canceled') { throw 'Engine cancellation was not acknowledged' }
$results.Add(@{name='engine-build-cancellation';passed=$true;exitCode=$p.ExitCode})
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$testRoot/results.json" -Encoding UTF8
Write-Host "Runtime smoke tests passed: $testRoot"
