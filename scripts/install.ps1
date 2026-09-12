param([string]$BuildToolsPath='D:\BuildTools\VS2022', [switch]$SkipBuildTools)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
$projectRoot=Split-Path $PSScriptRoot -Parent
$localDeps=Join-Path $projectRoot '.local'
$downloads=Join-Path $localDeps 'downloads'
New-Item -ItemType Directory -Force -Path $downloads | Out-Null
function Download-Verified([string]$Url,[string]$File,[string]$Hash) {
    if (!(Test-Path -LiteralPath $File) -or ($Hash -and (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -ne $Hash)) {
        Write-Host "Downloading $Url"
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile "$File.partial" -TimeoutSec 1800
        if ($Hash -and (Get-FileHash -LiteralPath "$File.partial" -Algorithm SHA256).Hash -ne $Hash) { throw "Checksum mismatch: $File" }
        Move-Item -LiteralPath "$File.partial" -Destination $File -Force
    }
}
function Expand-Zip([string]$Archive,[string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (!(Test-Path -LiteralPath $Destination)) { New-Item -ItemType Directory -Path $Destination | Out-Null }
    $zip=[IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $root=[IO.Path]::GetFullPath($Destination).TrimEnd('\')+'\'
        foreach($entry in $zip.Entries) {
            $target=[IO.Path]::GetFullPath((Join-Path $Destination $entry.FullName))
            if (!$target.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe archive path' }
            if (!$entry.Name) { New-Item -ItemType Directory -Force -Path $target | Out-Null; continue }
            New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$true)
        }
    } finally { $zip.Dispose() }
}
$vswhere=Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath=$null
if(Test-Path -LiteralPath $vswhere) {
    $vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.14.38.17.8.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
}
if(!$SkipBuildTools -and !$vsPath) {
    $installer=Join-Path $downloads 'vs_buildtools.exe'
    Download-Verified 'https://aka.ms/vs/17/release/vs_buildtools.exe' $installer ''
    $signature=Get-AuthenticodeSignature -LiteralPath $installer
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') { throw 'Invalid Microsoft installer signature' }
    $arguments='--quiet --wait --norestart --nocache --installPath "{0}" --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.14.38.17.8.x86.x64 --add Microsoft.VisualStudio.Component.Windows10SDK.19041 --add Microsoft.VisualStudio.Component.VC.CMake.Project' -f $BuildToolsPath
    $p=Start-Process -FilePath $installer -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru -Wait
    if($p.ExitCode -notin @(0,3010)) { throw "Build Tools installation failed: $($p.ExitCode)" }
}
$packages=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cuda-packages.json') -Raw | ConvertFrom-Json
foreach($package in $packages) {
    $archive=Join-Path $downloads ($package.name+'.zip')
    Download-Verified $package.url $archive $package.sha256
    $destination=Join-Path $localDeps ('cuda-packages\'+$package.name)
    $marker=Join-Path $destination '.installed'
    if(!(Test-Path -LiteralPath $marker)) {
        Expand-Zip $archive $destination
        $folder=Get-ChildItem -LiteralPath $destination -Directory | Select-Object -First 1
        if(!$folder) { throw "Missing extracted package: $($package.name)" }
        New-Item -ItemType Directory -Force -Path "$localDeps\cuda" | Out-Null
        foreach($source in (Get-ChildItem -LiteralPath $folder.FullName -Recurse -File)) {
            $relative=$source.FullName.Substring($folder.FullName.Length+1)
            $target=Join-Path "$localDeps\cuda" $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
            if((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target).Hash -eq (Get-FileHash -LiteralPath $source.FullName).Hash) { continue }
            Copy-Item -LiteralPath $source.FullName -Destination $target -Force
        }
        Set-Content -LiteralPath $marker -Value $package.sha256
    }
}
$opencv=Join-Path $downloads 'opencv-4.10.0-windows.exe'
Download-Verified 'https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-windows.exe' $opencv 'BFF38466091C313DAC21A0B73EEA8278316A89C1D434C6F0B10697E087670168'
if(!(Test-Path -LiteralPath "$localDeps\opencv-dist\opencv\build\x64\vc16\bin\opencv_world4100.dll")) {
    $arguments='-y -o"{0}"' -f "$localDeps\opencv-dist"
    $p=Start-Process -FilePath $opencv -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if($p.ExitCode -ne 0) { throw 'OpenCV extraction failed' }
}
$wheel=Join-Path $downloads 'tensorrt-10.6.0.post2.whl'
Download-Verified 'https://pypi.nvidia.com/tensorrt-cu12-libs/tensorrt_cu12_libs-10.6.0.post2-py2.py3-none-win_amd64.whl' $wheel 'a8542e59d75d8a30df792782442f4c4f0a05bc6960d3964c6b7fb641552d1fd8'
if(!(Test-Path -LiteralPath "$localDeps\tensorrt\tensorrt_libs\nvinfer_10.dll")) { Expand-Zip $wheel "$localDeps\tensorrt" }
$headers=Join-Path $downloads 'tensorrt-source.zip'
Download-Verified 'https://codeload.github.com/NVIDIA/TensorRT/zip/refs/heads/release/10.6' $headers '708E619530774DFF42EDE7F8FC536ECE99705B861FF32A3E1BA97A2007A5F5BA'
if(!(Test-Path -LiteralPath "$localDeps\tensorrt\include\NvInfer.h")) {
    Expand-Zip $headers "$localDeps\tensorrt-source"
    New-Item -ItemType Directory -Force -Path "$localDeps\tensorrt\include" | Out-Null
    Get-ChildItem -Path "$localDeps\tensorrt-source\TensorRT-release-10.6\include\*.h" | Copy-Item -Destination "$localDeps\tensorrt\include" -Force
}
Download-Verified 'https://raw.githubusercontent.com/onnx/onnx-tensorrt/10.6-GA/NvOnnxParser.h' "$localDeps\tensorrt\include\NvOnnxParser.h" '20274B41C594B798A14AF2B14ABF8F5BCCF293E0E9CA6432DB116358B6EA1B19'
Write-Host 'Dependencies ready. Run scripts/build.ps1. The NVIDIA driver and model are not modified.'
