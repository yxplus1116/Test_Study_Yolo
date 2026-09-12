$ErrorActionPreference='Stop'
$projectRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $projectRoot 'scripts/menu-session.ps1')
$script:checks=0
function Check([bool]$Condition,[string]$Description) {
    if (!$Condition) { throw "FAIL: $Description" }
    ++$script:checks
}
$settings=[pscustomobject]@{capture='gdi';monitor=2;size=416}
$routes=@(Get-MenuRoutes)
Check ($routes.Count -eq 13) 'all menu entries'
Check ((@($routes.Number | Sort-Object -Unique)).Count -eq 13) 'unique route numbers'
foreach($number in 0..12) {
    $plan=Get-MenuPlan $number $settings 'C:\示例 文件\input image.png'
    Check ($plan.number -eq $number) "route $number"
    Check (($plan.arguments -contains '--enable-input') -eq ($number -in @(3,4))) "only explicit control routes inject: $number"
}
foreach($pair in @(@(1,'1'),@(2,'0'),@(3,'1'),@(4,'0'))) {
    $plan=Get-MenuPlan $pair[0] $settings
    $index=[Array]::IndexOf($plan.arguments,'--label')
    Check ($plan.arguments[$index+1] -eq $pair[1]) "class routing $($pair[0])"
    Check ($plan.arguments -contains '2') 'selected monitor propagated'
}
foreach($kind in @('gpu','gdi','dxgi')) {
    $plan=Get-MenuPlan 7 $settings '' $kind
    Check (!$plan.inputEnabled) "benchmark no injection: $kind"
    Check ($plan.arguments -contains '--headless') "benchmark no display: $kind"
    if($kind -eq 'gpu'){Check ($plan.arguments -contains '--benchmark') 'GPU benchmark flag'}
    else {Check ($plan.arguments -contains $kind) "capture benchmark: $kind"}
}
$threw=$false
try {Get-MenuPlan 99 $settings | Out-Null} catch {$threw=$true}
Check $threw 'invalid route rejected'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativeArgTest {
    [DllImport("shell32.dll", SetLastError=true)]
    static extern IntPtr CommandLineToArgvW([MarshalAs(UnmanagedType.LPWStr)] string command, out int count);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    public static string[] Parse(string command) {
        int count; var p=CommandLineToArgvW(command,out count);
        if(p==IntPtr.Zero) throw new Exception("parse failed");
        try {
            var result=new string[count];
            for(int i=0;i<count;i++) result[i]=Marshal.PtrToStringUni(Marshal.ReadIntPtr(p,i*IntPtr.Size));
            return result;
        } finally {LocalFree(p);}
    }
}
'@
$values=@('', 'plain', 'C:\path with spaces\', 'C:\中文 示例\photo.png', 'a"b', 'x\\"y', '$(never execute)', 'a&b|c')
$line='program '+(($values|ForEach-Object {ConvertTo-NativeArgument $_}) -join ' ')
$parsed=[NativeArgTest]::Parse($line)
Check ($parsed.Length -eq $values.Count+1) 'quoted argument count'
for($i=0;$i -lt $values.Count;++$i){Check ($parsed[$i+1] -ceq $values[$i]) "argument quoting roundtrip $i"}

$fixture=Join-Path $projectRoot ('.local/menu-tests-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $fixture '.local/runlogs/session') -Force | Out-Null
$recordPath=Join-Path $fixture '.local/live-session.json'
$script:fakeProcess=[pscustomobject]@{Id=12345;Path=(Join-Path $fixture 'dist/TestStudyYolo.exe');StartTime=(Get-Date)}
$script:fakeProcess|Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {param($Milliseconds) return $true}
function Get-Process { param([int]$Id) if($Id -eq 12345){return $script:fakeProcess} }
try {
    $record=@{pid=12345;startTicks=$script:fakeProcess.StartTime.ToUniversalTime().Ticks;stopFile=(Join-Path $fixture '.local/runlogs/session/stop.request');output=(Join-Path $fixture '.local/runlogs/session')}
    $record|ConvertTo-Json|Set-Content -LiteralPath $recordPath -Encoding UTF8
    Check ((Get-ManagedSession $fixture).state -eq 'running') 'legacy preview identity recognized'
    Stop-ManagedSession $fixture
    Check (Test-Path -LiteralPath $record.stopFile) 'graceful stop uses validated file'
    $record.stopFile=Join-Path $fixture 'outside-stop.request'
    $record|ConvertTo-Json|Set-Content -LiteralPath $recordPath -Encoding UTF8
    $threw=$false
    try{Stop-ManagedSession $fixture}catch{$threw=$true}
    Check $threw 'stop outside runlogs rejected'
    Check (!(Test-Path -LiteralPath $record.stopFile)) 'outside stop path untouched'
    $record.startTicks++
    $record|ConvertTo-Json|Set-Content -LiteralPath $recordPath -Encoding UTF8
    Check ((Get-ManagedSession $fixture).state -eq 'mismatch') 'reused PID rejected'
    $record.pid=12346
    $record|ConvertTo-Json|Set-Content -LiteralPath $recordPath -Encoding UTF8
    Check ((Get-ManagedSession $fixture).state -eq 'exited') 'exited session recognized'
    Set-Content -LiteralPath $recordPath -Value 'broken JSON' -Encoding UTF8
    Check ((Get-ManagedSession $fixture).state -eq 'invalid') 'malformed session rejected'
} finally {Remove-Item -LiteralPath Function:\Get-Process}
foreach($file in @('menu.ps1','menu-session.ps1','menu-watchdog.ps1')) {
    $path=Join-Path $projectRoot ('scripts/'+$file)
    $tokens=$null;$errors=$null
    [void][Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
    Check ($errors.Count -eq 0) "PS parser $file"
    $bytes=[IO.File]::ReadAllBytes($path)
    Check ($bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) "UTF8 BOM $file"
}
Write-Host "PASS: $script:checks checks. No application or mouse-control process was launched."
