[CmdletBinding()]
param(
    [switch]$List,
    [switch]$DryRun,
    [int]$Choice=-1,
    [string]$MediaPath='',
    [ValidateSet('gpu','gdi','dxgi','all')][string]$Performance='gpu'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'menu-session.ps1')
$settings = Read-MenuSettings $projectRoot
$script:pendingPerformance = New-Object 'System.Collections.Generic.Queue[string]'
$script:performanceActive = $false
$script:performanceResults = @()
$script:lastMessage = ''
$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

if ($List) {
    Get-MenuRoutes | ConvertTo-Json -Depth 4
    exit 0
}
if ($DryRun) {
    if ($Choice -lt 0) { throw '-DryRun requires -Choice.' }
    if ($Choice -eq 7 -and $Performance -eq 'all') {
        @('gpu','gdi','dxgi') | ForEach-Object { Get-MenuPlan 7 $settings '' $_ } | ConvertTo-Json -Depth 5
    } else {
        Get-MenuPlan $Choice $settings $MediaPath $Performance | ConvertTo-Json -Depth 5
    }
    exit 0
}
if ($Choice -ge 0) { throw '-Choice is for -DryRun routing checks. Open the menu without parameters for normal use.' }

function Show-Result([object]$Session) {
    if (!$Session -or !$Session.output) { return }
    $path = Join-Path $Session.output 'summary.json'
    if (Test-Path -LiteralPath $path) {
        try {
            $result = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            if ($result.mode -eq 'gpu_benchmark') {
                Write-Host ("上次结果：GPU 模型 {0:N1} FPS / 每次 {1:N2} ms（不含采集与预览）" -f $result.gpu_fps,$result.gpu_mean_ms) -ForegroundColor Cyan
            } else {
                Write-Host ("上次结果：端到端 {0:N1} FPS，共 {1} 帧；采集错误 {2}" -f $result.fps,$result.frames,$result.capture_errors) -ForegroundColor Cyan
                if ($result.capture -eq 'dxgi' -and $result.empty_captures -gt 0) {
                    Write-Host ("DXGI 有 {0} 次空采样；静止桌面会等待新画面，这个 FPS 不代表 GPU 推理速度。" -f $result.empty_captures) -ForegroundColor DarkGray
                }
            }
        } catch { Write-Host '任务结果仍在写入，稍后刷新。' }
    }
}
function Show-Menu {
    Clear-Host
    Write-Host '════════════════════════════════════════════════════════════' -ForegroundColor Cyan
    Write-Host '       TestStudyYolo · 本机学习与验证控制台' -ForegroundColor Cyan
    Write-Host '════════════════════════════════════════════════════════════' -ForegroundColor Cyan
    $session = Get-ManagedSession $projectRoot
    if ($session.state -eq 'running') {
        $name = if ($session.data.name) {$session.data.name} else {'原预览启动器任务'}
        $mode = if ($session.data.inputEnabled) {'鼠标输出已开启'} elseif ($null -ne $session.data.inputEnabled) {'鼠标输出关闭'} else {'启动方式：旧预览脚本（只检测）'}
        Write-Host "运行中：$name | $mode | PID $($session.process.Id)" -ForegroundColor Green
        $log = Join-Path $session.data.output 'stdout.log'
        if (Test-Path -LiteralPath $log) {
            $line = @(Get-Content -LiteralPath $log -Tail 1 -ErrorAction SilentlyContinue)
            if ($line.Count) { Write-Host $line[-1] -ForegroundColor DarkGray }
        }
    } elseif ($session.state -in @('invalid','mismatch')) {
        Write-Host '任务记录需要检查，菜单不会操作身份不匹配的进程。' -ForegroundColor Yellow
    } else {
        Write-Host '当前状态：空闲' -ForegroundColor Gray
        Show-Result $session.data
    }
    $other = @(Get-UnmanagedInstances $projectRoot $session)
    if ($other.Count) { Write-Host "另有直接启动的实例：PID $($other.Id -join ', ')；请先在原窗口退出。" -ForegroundColor Yellow }
    Write-Host ("下次实时采集：{0} | 显示器 {1} | 中心区域 {2} × {2}" -f $settings.capture.ToUpper(),$settings.monitor,$settings.size)
    Write-Host ''
    Write-Host '  1  头部识别预览                 2  身体识别预览'
    Write-Host '  3  头部识别 + 鼠标控制          4  身体识别 + 鼠标控制'
    Write-Host '  5  选择图片识别                 6  选择视频识别'
    Write-Host '  7  性能测试                     8  桌面鼠标标定'
    Write-Host '  9  采集 / 显示器设置           10  停止当前任务'
    Write-Host ' 11  日志 / 结果 / 说明           12  编译 / 安装维护'
    Write-Host '  0  停止任务并退出'
    Write-Host ''
    Write-Host '选择 3 / 4 后，还须按住鼠标左键或右键；松开即停止输出。' -ForegroundColor Yellow
    Write-Host '控制误差来自“目标与画面中心”，静态图片不会随光标移动。' -ForegroundColor DarkGray
    Write-Host '预览快捷键：↑头部 ↓身体 ←暂停 →恢复 ESC退出。' -ForegroundColor DarkGray
    Write-Host '日志自动保存；启动新任务会先停止旧任务。回车可刷新状态。' -ForegroundColor DarkGray
    if ($script:performanceActive) {
        Write-Host "性能对比进行中，后续还有 $($script:pendingPerformance.Count) 项；选择 10 可停止整组。" -ForegroundColor Cyan
        Write-Host '停留在此主菜单会自动继续下一项；进入子菜单时会等待返回。' -ForegroundColor DarkGray
    }
    if ($script:lastMessage) { Write-Host ''; Write-Host $script:lastMessage -ForegroundColor Cyan }
    Write-Host ''
}
function Select-Media([bool]$Video) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = if ($Video) {'选择要检测的视频'} else {'选择要检测的图片'}
    $dialog.Filter = if ($Video) {'视频文件|*.mp4;*.avi;*.mkv;*.mov;*.wmv|所有文件|*.*'} else {'图片文件|*.jpg;*.jpeg;*.png;*.bmp;*.webp|所有文件|*.*'}
    $dialog.InitialDirectory = Join-Path $projectRoot 'workspace'
    $dialog.CheckFileExists = $true
    try {
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.FileName }
        return ''
    } finally { $dialog.Dispose() }
}
function Start-Plan([object]$Plan) {
    if ($Plan.number -in @(3,4)) {
        Write-Host '已选择真实鼠标控制。请将鼠标左/右键松开；按住时才输出，ESC 或菜单 10 停止。' -ForegroundColor Yellow
    }
    if ($Plan.number -eq 8) {
        Write-Host '标定用于测量“鼠标输入单位 → 桌面光标像素”的比例，不训练模型或选择目标。'
        Write-Host '开始后有 2 秒准备时间。将光标放在离屏幕边缘 128 像素以上的位置并保持静止。' -ForegroundColor Yellow
        Write-Host '程序会进行小幅往返移动；ESC 或菜单 10 取消。'
    }
    $session = Start-ManagedSession $projectRoot $Plan $PID
    $script:lastMessage = "已启动：$($Plan.name)。日志保存在 $($session.output)"
    return $session
}
function Stop-PerformanceQueue {
    $script:pendingPerformance.Clear()
    $script:performanceActive = $false
}
function Update-PerformanceQueue {
    if (!$script:performanceActive) { return $false }
    $session = Get-ManagedSession $projectRoot
    if ($session.state -eq 'running') { return $false }
    if ($session.data -and $session.data.kind -eq 'performance') {
        $summaryFile = Join-Path $session.data.output 'summary.json'
        if (Test-Path -LiteralPath $summaryFile) {
            $result = Get-Content -LiteralPath $summaryFile -Raw | ConvertFrom-Json
            $script:performanceResults += [pscustomobject]@{name=$session.data.name;summary=$result;output=$session.data.output}
        } else {
            Stop-PerformanceQueue
            $script:lastMessage = "性能测试未正常完成，已停止对比队列。请选 11 查看 $($session.data.output) 中的错误日志。"
            return $true
        }
    }
    if ($script:pendingPerformance.Count) {
        $kind = $script:pendingPerformance.Dequeue()
        try { [void](Start-Plan (Get-MenuPlan 7 $settings '' $kind)) }
        catch { Stop-PerformanceQueue; $script:lastMessage = $_.Exception.Message }
    } else {
        Stop-PerformanceQueue
        $parts = @($script:performanceResults | ForEach-Object {
            if ($_.summary.mode -eq 'gpu_benchmark') { 'GPU 模型 {0:N1} FPS' -f $_.summary.gpu_fps }
            else { '{0}：{1:N1} FPS' -f $_.name,$_.summary.fps }
        })
        $script:lastMessage = '测试完成：' + ($parts -join '；') + '。GPU 模型与端到端 FPS 的统计范围不同。'
        if ($script:performanceResults.Count) {
            $script:performanceResults | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $projectRoot '.local/performance-comparison.json') -Encoding UTF8
        }
    }
    return $true
}
function Read-MenuNumber {
    Write-Host '请输入编号并按回车：' -NoNewline
    if ([Console]::IsInputRedirected) {
        $line = [Console]::ReadLine()
        if ($null -eq $line) { return '0' }
        return $line
    }
    $buffer = ''
    while ($true) {
        if (Update-PerformanceQueue) {
            Show-Menu
            Write-Host ('请输入编号并按回车：' + $buffer) -NoNewline
        }
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq [ConsoleKey]::Enter) { Write-Host ''; return $buffer }
            if ($key.Key -eq [ConsoleKey]::Escape) { Write-Host ''; return '10' }
            if ($key.Key -eq [ConsoleKey]::Backspace -and $buffer.Length) {
                $buffer = $buffer.Substring(0,$buffer.Length-1)
                [Console]::Write([char]8); [Console]::Write(' '); [Console]::Write([char]8)
            } elseif ([char]::IsDigit($key.KeyChar) -and $buffer.Length -lt 2) {
                $buffer += $key.KeyChar
                Write-Host $key.KeyChar -NoNewline
            }
        }
        Start-Sleep -Milliseconds 100
    }
}
function Edit-Settings {
    Write-Host ' 1 GDI    2 DXGI    3 选择显示器    4 采集尺寸    5 恢复默认    0 返回'
    $option = Read-Host '设置编号'
    switch ($option) {
        '1' { $settings.capture = 'gdi' }
        '2' { $settings.capture = 'dxgi' }
        '3' {
            $lines = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run.ps1') --list-monitors 2>&1)
            $lines | ForEach-Object { Write-Host $_ }
            if ($LASTEXITCODE -ne 0) { throw '无法列出显示器。' }
            $indices = @($lines | ForEach-Object { if ([string]$_ -match '^(\d+):') { [int]$Matches[1] } })
            $value = Read-Host '请输入上面列出的显示器编号'
            $parsed = 0
            if (![int]::TryParse($value,[ref]$parsed) -or $parsed -notin $indices) { throw '显示器编号不在列表中。' }
            $settings.monitor = $parsed
        }
        '4' {
            $value = Read-Host '中心正方形边长（32 至 4096，默认 416；应不超过显示器尺寸）'
            $parsed = 0
            if (![int]::TryParse($value,[ref]$parsed) -or $parsed -lt 32 -or $parsed -gt 4096) { throw '采集尺寸超出有效范围。' }
            $settings.size = $parsed
        }
        '5' { $settings.capture='gdi';$settings.monitor=0;$settings.size=416 }
        default { return }
    }
    New-Item -ItemType Directory -Path (Join-Path $projectRoot '.local') -Force | Out-Null
    $settings | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot '.local/menu-settings.json') -Encoding UTF8
    $script:lastMessage = '设置已保存，下次启动任务时生效。'
}
function Show-Logs {
    $session = Get-ManagedSession $projectRoot
    Write-Host ' 1 查看本次日志    2 打开日志文件夹    3 打开最后识别图'
    Write-Host ' 4 阅读项目说明    5 查看性能对比结果'
    Write-Host ' 6 鼠标失效排查说明                 0 返回'
    $option = Read-Host '查看编号'
    if ($option -eq '4') { Invoke-Item -LiteralPath (Join-Path $projectRoot 'README.md');return }
    if ($option -eq '6') { Invoke-Item -LiteralPath (Join-Path $projectRoot 'docs/input-troubleshooting.md');return }
    if ($option -eq '5') {
        $path = Join-Path $projectRoot '.local/performance-comparison.json'
        if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path | Out-Host } else { Write-Host '还没有性能对比结果，请先选菜单 7。' }
        [void](Read-Host '按回车返回');return
    }
    if ($option -eq '0') { return }
    if (!$session.data -or !$session.data.output) { Write-Host '尚无任务日志。';[void](Read-Host '按回车返回');return }
    $output = [IO.Path]::GetFullPath([string]$session.data.output)
    $allowed = [IO.Path]::GetFullPath((Join-Path $projectRoot '.local/runlogs')).TrimEnd('\') + '\'
    if (!$output.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) { throw '日志目录不在项目允许范围内。' }
    if ($option -eq '1') {
        foreach ($file in @('stdout.log','stderr.log')) {
            Write-Host $file -ForegroundColor Cyan
            $path = Join-Path $output $file
            if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path -Tail 15 | Out-Host }
        }
        [void](Read-Host '按回车返回')
    } elseif ($option -eq '2') { Invoke-Item -LiteralPath $output }
    elseif ($option -eq '3') {
        $path = Join-Path $output 'last-frame.jpg'
        if (!(Test-Path -LiteralPath $path)) { throw '最后识别图在任务停止后保存，请先选 10。' }
        Invoke-Item -LiteralPath $path
    }
}
function Invoke-Maintenance {
    Write-Host ' 1 编译并运行核心测试    2 安装 / 补齐依赖    0 返回'
    $option = Read-Host '维护编号'
    if ($option -notin @('1','2')) { return }
    Stop-PerformanceQueue
    $managed = Get-ManagedSession $projectRoot
    if (@(Get-UnmanagedInstances $projectRoot $managed).Count) { throw '请先关闭直接启动的项目实例，再进行维护。' }
    Stop-ManagedSession $projectRoot
    $scriptFile = if ($option -eq '1') {'build.ps1'} else {'install.ps1'}
    Write-Host '正在维护，日志显示在此窗口；完成后回到菜单。' -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $scriptFile)
    if ($LASTEXITCODE -ne 0) { throw "维护失败（退出码 $LASTEXITCODE），请查看上方日志。" }
    [void](Read-Host '维护完成，按回车返回')
}

$sha = [Security.Cryptography.SHA256]::Create()
try { $key = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($projectRoot.ToLowerInvariant()))).Replace('-','').Substring(0,24) }
finally { $sha.Dispose() }
$mutex = New-Object Threading.Mutex($false,("Local\TestStudyYolo.Menu."+$key))
$ownsMutex = $false
try {
    try { $ownsMutex = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $ownsMutex=$true }
    if (!$ownsMutex) { Write-Host '此项目的控制菜单已经打开，请使用原菜单窗口。';[void](Read-Host '按回车关闭此窗口');exit 1 }
    $Host.UI.RawUI.WindowTitle = 'TestStudyYolo - 本机控制台'
    $finished = $false
    while (!$finished) {
        Show-Menu
        $choiceText = Read-MenuNumber
        if ($choiceText -eq '') { continue }
        $number = 0
        if (![int]::TryParse($choiceText,[ref]$number)) { $script:lastMessage='请输入菜单中的编号。';continue }
        try {
            if ($number -ge 1 -and $number -le 6) {
                Stop-PerformanceQueue
                $selected = ''
                if ($number -in @(5,6)) { $selected = Select-Media ($number -eq 6);if (!$selected) {continue} }
                [void](Start-Plan (Get-MenuPlan $number $settings $selected))
            } else {
                switch ($number) {
                    0 { Stop-PerformanceQueue;Stop-ManagedSession $projectRoot;$finished=$true }
                    7 {
                        Write-Host ' 1 GPU 模型（不含采集）   2 GDI 全链路   3 DXGI 全链路   4 顺序对比全部   0 返回'
                        Write-Host '全链路测试各 8 秒且关闭预览；DXGI 依赖桌面更新，测试时保持被测内容在变化。'
                        $selected = Read-Host '测试编号'
                        $kinds = switch ($selected) {'1' {@('gpu')} '2' {@('gdi')} '3' {@('dxgi')} '4' {@('gpu','gdi','dxgi')} default {@()}}
                        if ($kinds.Count) {
                            Stop-PerformanceQueue
                            Stop-ManagedSession $projectRoot
                            $script:performanceResults=@()
                            foreach ($kind in $kinds) {$script:pendingPerformance.Enqueue($kind)}
                            $first=$script:pendingPerformance.Dequeue()
                            [void](Start-Plan (Get-MenuPlan 7 $settings '' $first))
                            $script:performanceActive=$true
                        }
                    }
                    8 { Stop-PerformanceQueue;[void](Start-Plan (Get-MenuPlan 8 $settings)) }
                    9 { Edit-Settings }
                    10 { Stop-PerformanceQueue;Stop-ManagedSession $projectRoot;$script:lastMessage='当前菜单管理的任务已停止。直接 CLI 启动的实例须在原窗口退出。' }
                    11 { Show-Logs }
                    12 { Invoke-Maintenance }
                    default { $script:lastMessage='没有这个编号，请选择 0 至 12。' }
                }
            }
        } catch {
            $script:lastMessage=$_.Exception.Message
            Write-Host $script:lastMessage -ForegroundColor Red
            [void](Read-Host '按回车返回菜单')
        }
    }
} finally {
    if ($ownsMutex) {
        $session = Get-ManagedSession $projectRoot
        if ($session.state -eq 'running' -and $session.data.ownerPid -eq $PID) {
            try { Stop-ManagedSession $projectRoot } catch { Write-Warning $_.Exception.Message }
        }
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
}
