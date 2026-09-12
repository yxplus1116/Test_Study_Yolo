param([switch]$SkipTests, [switch]$Clean, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Run scripts/install.ps1 first (MSVC missing).' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'Visual C++ tools are not installed.' }
$cmakeRoot = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Join-Path $cmakeRoot 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeRoot 'Ninja\ninja.exe'
$localDeps = Join-Path $projectRoot '.local'
$cleanStep = ''
if ($Clean -or $Rebuild) { $cleanStep = '"{0}" --build build --target clean' -f $cmake }
$finishStep = '"{0}" --build build --parallel 4' -f $cmake
if ($Clean) { $finishStep = 'exit /b 0' }
New-Item -ItemType Directory -Force -Path "$localDeps\import-libs" | Out-Null
$buildBatch = @"
@echo off
chcp 65001 >nul
set VSLANG=1033
call "$vsPath\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.38
if errorlevel 1 exit /b 1
cd /d "$projectRoot"
lib.exe /nologo /machine:x64 /def:cmake\nvinfer.def /out:.local\import-libs\nvinfer.lib
if errorlevel 1 exit /b 1
lib.exe /nologo /machine:x64 /def:cmake\nvonnxparser.def /out:.local\import-libs\nvonnxparser.lib
if errorlevel 1 exit /b 1
"$cmake" -S . -B build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
$cleanStep
if errorlevel 1 exit /b 1
$finishStep
exit /b %errorlevel%
"@
$batchPath = Join-Path $localDeps 'build.cmd'
Set-Content -LiteralPath $batchPath -Value $buildBatch -Encoding ASCII
& $env:ComSpec /d /c $batchPath
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
if ($Clean) { exit 0 }
$dependencies = & $ninja -C (Join-Path $projectRoot 'build') -t deps CMakeFiles/TestStudyYolo.dir/src/main.cpp.obj
if ($LASTEXITCODE -ne 0 -or !($dependencies -match 'application[/\\]config.h')) {
    throw 'Ninja did not record application header dependencies. Rebuild after fixing MSVC output encoding.'
}
$env:PATH = "$localDeps\opencv-dist\opencv\build\x64\vc16\bin;$env:PATH"
if (!$SkipTests) {
    & (Join-Path $cmakeRoot 'CMake\bin\ctest.exe') --test-dir (Join-Path $projectRoot 'build') --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed.' }
}
$dist = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item -LiteralPath "$projectRoot\build\TestStudyYolo.exe" -Destination $dist -Force
Write-Host "Deployed to $dist. Open start.cmd for the menu; scripts/run.ps1 is the advanced CLI."
