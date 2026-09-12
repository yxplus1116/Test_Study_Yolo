# Dot-sourced by the menu and its tests. Importing it never starts a process.
function ConvertTo-NativeArgument([AllowEmptyString()][string]$Value) {
    if ($Value -notmatch '[\s"]' -and $Value.Length) { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}
function Get-MenuRoutes {
    @(
        [pscustomobject]@{ Number=1; Name='头部识别预览'; Kind='head-preview'; Label=1; Input=$false }
        [pscustomobject]@{ Number=2; Name='身体识别预览'; Kind='body-preview'; Label=0; Input=$false }
        [pscustomobject]@{ Number=3; Name='头部识别 + 鼠标控制'; Kind='head-control'; Label=1; Input=$true }
        [pscustomobject]@{ Number=4; Name='身体识别 + 鼠标控制'; Kind='body-control'; Label=0; Input=$true }
        [pscustomobject]@{ Number=5; Name='选择图片进行识别'; Kind='image'; Label=1; Input=$false }
        [pscustomobject]@{ Number=6; Name='选择视频进行识别'; Kind='video'; Label=1; Input=$false }
        [pscustomobject]@{ Number=7; Name='性能测试：GPU / GDI / DXGI'; Kind='performance'; Label=1; Input=$false }
        [pscustomobject]@{ Number=8; Name='标定桌面鼠标响应'; Kind='calibration'; Label=-1; Input=$true }
        [pscustomobject]@{ Number=9; Name='采集方式 / 显示器 / 采集尺寸'; Kind='settings'; Label=-1; Input=$false }
        [pscustomobject]@{ Number=10; Name='停止当前任务'; Kind='stop'; Label=-1; Input=$false }
        [pscustomobject]@{ Number=11; Name='查看日志 / 结果 / 项目说明'; Kind='logs'; Label=-1; Input=$false }
        [pscustomobject]@{ Number=12; Name='维护：编译 / 安装依赖'; Kind='maintenance'; Label=-1; Input=$false }
        [pscustomobject]@{ Number=0; Name='停止任务并退出'; Kind='exit'; Label=-1; Input=$false }
    )
}
function Get-MenuPlan {
    param([int]$Number, [object]$Settings, [string]$MediaPath='', [string]$Performance='gpu')
    $route = Get-MenuRoutes | Where-Object Number -eq $Number
    if (!$route) { throw "不存在菜单项 $Number。" }
    $arguments = @()
    $description = $route.Name
    if ($Number -ge 1 -and $Number -le 4) {
        $arguments = @('--capture',[string]$Settings.capture,'--monitor',[string]$Settings.monitor,'--size',[string]$Settings.size,'--label',[string]$route.Label)
        if ($route.Input) { $arguments += '--enable-input' }
    } elseif ($Number -eq 5 -or $Number -eq 6) {
        if (!$MediaPath) { throw '请先选择一个文件。' }
        $arguments = @($(if ($Number -eq 5) {'--image'} else {'--video'}), $MediaPath, '--label','1')
        if ($Number -eq 5) { $arguments += @('--seconds','20') }
    } elseif ($Number -eq 7) {
        if ($Performance -notin @('gpu','gdi','dxgi')) { throw '性能测试类型必须是 gpu、gdi 或 dxgi。' }
        $arguments = @('--headless','--label','1')
        if ($Performance -eq 'gpu') {
            $arguments += @('--benchmark','--frames','300')
            $description = 'GPU 模型基准（300 帧）'
        } else {
            $arguments += @('--capture',$Performance,'--monitor',[string]$Settings.monitor,'--size',[string]$Settings.size,'--seconds','8')
            $description = "$($Performance.ToUpper()) 端到端基准（8 秒）"
        }
    } elseif ($Number -eq 8) { $arguments = @('--calibrate') }
    [pscustomobject]@{number=$Number;name=$description;kind=$route.Kind;label=$route.Label;inputEnabled=$route.Input;arguments=[string[]]$arguments;performance=$Performance}
}
function Read-MenuSettings([string]$ProjectRoot) {
    $defaults = [pscustomobject]@{ capture='gdi'; monitor=0; size=416 }
    $path = Join-Path $ProjectRoot '.local/menu-settings.json'
    if (Test-Path -LiteralPath $path) {
        try {
            $value = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            if ($value.capture -notin @('gdi','dxgi') -or $null -eq $value.monitor -or $null -eq $value.size -or [int]$value.monitor -lt 0 -or [int]$value.size -lt 32 -or [int]$value.size -gt 4096) { throw 'Invalid settings' }
            return [pscustomobject]@{capture=[string]$value.capture;monitor=[int]$value.monitor;size=[int]$value.size}
        } catch { Write-Warning '菜单设置损坏，已采用 GDI / 显示器 0 / 416 像素。' }
    }
    return $defaults
}
function Get-ManagedSession([string]$ProjectRoot) {
    $path = Join-Path $ProjectRoot '.local/live-session.json'
    if (!(Test-Path -LiteralPath $path)) { return [pscustomobject]@{state='missing';data=$null;process=$null} }
    try { $data = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json }
    catch { return [pscustomobject]@{state='invalid';data=$null;process=$null} }
    if (!$data.pid -or !$data.startTicks) { return [pscustomobject]@{state='invalid';data=$data;process=$null} }
    $process = Get-Process -Id $data.pid -ErrorAction SilentlyContinue
    if (!$process) { return [pscustomobject]@{state='exited';data=$data;process=$null} }
    $expected = [IO.Path]::GetFullPath((Join-Path $ProjectRoot 'dist/TestStudyYolo.exe'))
    if (!$process.Path -or $process.Path -ne $expected -or $process.StartTime.ToUniversalTime().Ticks -ne $data.startTicks) { return [pscustomobject]@{state='mismatch';data=$data;process=$null} }
    return [pscustomobject]@{state='running';data=$data;process=$process}
}
function Get-UnmanagedInstances([string]$ProjectRoot, [object]$Managed) {
    $expected = [IO.Path]::GetFullPath((Join-Path $ProjectRoot 'dist/TestStudyYolo.exe'))
    @(Get-Process -Name TestStudyYolo -ErrorAction SilentlyContinue | Where-Object {
        (!$_.Path -or $_.Path -eq $expected) -and !($Managed.state -eq 'running' -and $_.Id -eq $Managed.process.Id)
    })
}
function Stop-ManagedSession([string]$ProjectRoot) {
    $session = Get-ManagedSession $ProjectRoot
    if ($session.state -in @('invalid','mismatch')) { throw '任务记录无法验证，未操作任何进程。请先检查 .local/live-session.json。' }
    if ($session.state -ne 'running') { return }
    if (!$session.data.stopFile) { throw '任务记录缺少停止文件；请在预览窗口按 ESC。' }
    $stopFile = [IO.Path]::GetFullPath([string]$session.data.stopFile)
    $allowed = [IO.Path]::GetFullPath((Join-Path $ProjectRoot '.local/runlogs')).TrimEnd('\') + '\'
    if (!$stopFile.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) { throw '任务停止文件不在项目日志目录中，未操作任何进程。' }
    Set-Content -LiteralPath $stopFile -Value 'stop' -Encoding ASCII
    if (!$session.process.WaitForExit(10000)) { throw '任务尚未在 10 秒内退出，未启动第二个实例。请查看日志或在预览窗口按 ESC。' }
}
function Start-ManagedSession {
    param([string]$ProjectRoot, [object]$Plan, [int]$OwnerProcessId)
    $exe = [IO.Path]::GetFullPath((Join-Path $ProjectRoot 'dist/TestStudyYolo.exe'))
    if (!(Test-Path -LiteralPath $exe)) { throw '程序尚未编译。请先选择菜单 12 → 编译。' }
    $managed = Get-ManagedSession $ProjectRoot
    $other = @(Get-UnmanagedInstances $ProjectRoot $managed)
    if ($other.Count) { throw "已有直接启动的项目进程（PID $($other.Id -join ', ')）。请在其终端按 Ctrl+C 或预览按 ESC，避免重复控制。" }
    Stop-ManagedSession $ProjectRoot
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $output = Join-Path $ProjectRoot ".local/runlogs/menu-$($Plan.kind)-$stamp"
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $stopFile = Join-Path $output 'stop.request'
    $arguments = @($Plan.arguments) + @('--output',$output,'--stop-file',$stopFile)
    $localDeps = Join-Path $ProjectRoot '.local'
    $oldPath = $env:PATH
    $process = $null
    try {
        $env:PATH = "$localDeps\tensorrt\tensorrt_libs;$localDeps\cuda\bin;$localDeps\opencv-dist\opencv\build\x64\vc16\bin;$oldPath"
        $argumentLine = ($arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
        $process = Start-Process -FilePath $exe -WorkingDirectory $ProjectRoot -ArgumentList $argumentLine -WindowStyle Hidden -PassThru -RedirectStandardOutput "$output/stdout.log" -RedirectStandardError "$output/stderr.log"
        $owner = Get-Process -Id $OwnerProcessId
        $session = [ordered]@{pid=$process.Id;startTicks=$process.StartTime.ToUniversalTime().Ticks;stopFile=$stopFile;output=$output;executable=$exe;name=$Plan.name;kind=$Plan.kind;label=$Plan.label;inputEnabled=$Plan.inputEnabled;arguments=$arguments;ownerPid=$owner.Id;ownerStartTicks=$owner.StartTime.ToUniversalTime().Ticks;launched=(Get-Date).ToString('o')}
        $json = $session | ConvertTo-Json -Depth 4
        $json | Set-Content -LiteralPath (Join-Path $output 'session.json') -Encoding UTF8
        $sessionFile = Join-Path $ProjectRoot '.local/live-session.json'
        $json | Set-Content -LiteralPath "$sessionFile.tmp" -Encoding UTF8
        Move-Item -LiteralPath "$sessionFile.tmp" -Destination $sessionFile -Force
        $watchdogArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $ProjectRoot 'scripts/menu-watchdog.ps1'),'-SessionPath',(Join-Path $output 'session.json'))
        $watchdogLine = ($watchdogArgs | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
        Start-Process -FilePath 'powershell.exe' -ArgumentList $watchdogLine -WorkingDirectory $ProjectRoot -WindowStyle Hidden -RedirectStandardError "$output/watchdog-error.log" | Out-Null
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) {
            $errorText = Get-Content -LiteralPath "$output/stderr.log" -Tail 5 -ErrorAction SilentlyContinue
            if ($process.ExitCode -ne 0) { throw "程序退出：$($errorText -join ' ')。日志：$output" }
        }
        return [pscustomobject]$session
    } catch {
        if ($process -and !$process.HasExited) {
            Set-Content -LiteralPath $stopFile -Value 'startup-failed' -Encoding ASCII
            [void]$process.WaitForExit(10000)
        }
        throw
    } finally { $env:PATH = $oldPath }
}
