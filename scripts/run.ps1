param([Parameter(ValueFromRemainingArguments=$true)][string[]]$AppArguments)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$localDeps = Join-Path $projectRoot '.local'
$exe = Join-Path $projectRoot 'dist\TestStudyYolo.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'Run scripts/build.ps1 first.' }
$env:PATH = "$localDeps\tensorrt\tensorrt_libs;$localDeps\cuda\bin;$localDeps\opencv-dist\opencv\build\x64\vc16\bin;$env:PATH"
Push-Location -LiteralPath $projectRoot
try {
    & $exe @AppArguments
    $result = $LASTEXITCODE
} finally { Pop-Location }
exit $result
