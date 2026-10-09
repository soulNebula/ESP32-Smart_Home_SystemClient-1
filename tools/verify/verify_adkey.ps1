[CmdletBinding()]
param(
    [string]$Port    = '',
    [int]   $Seconds = 45,
    [string]$Replay  = '',
    [switch]$NoLog
)

$ErrorActionPreference = 'Stop'

$projectDir = (Get-Item (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))).FullName
$logDir     = Join-Path $projectDir 'logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

# 这几个是蓝牙口
$BT = @('COM3', 'COM4', 'COM8', 'COM9')

function Parse-AdkeyLog {
    param([string[]]$Lines)

    $reDown = [regex]'\[ADKEY\]\s+(\S+)\s+(DOWN|UP|CLICK|LONG)\s*\(.*?(?:触发读数|mv)\s*=\s*(-?\d+)\s*(?:mV)?'
    $reCal  = [regex]'标定:\s*键(\S+?)\s+按下期间最低\s*(-?\d+)\s*mV'

    $stat = @{}
    foreach ($k in @('1','2','3','4','OK')) {
        $stat[$k] = [pscustomobject]@{ Key = $k; Down = 0; Up = 0; Click = 0; Long = 0; Mv = @() ; CalMin = $null }
    }

    foreach ($ln in $Lines) {
        $m = $reDown.Match($ln)
        if ($m.Success) {
            $k = $m.Groups[1].Value
            if ($stat.ContainsKey($k)) {
                $ev = $m.Groups[2].Value
                $mv = [int]$m.Groups[3].Value
                switch ($ev) {
                    'DOWN'  { $stat[$k].Down++ }
                    'UP'    { $stat[$k].Up++ }
                    'CLICK' { $stat[$k].Click++ }
                    'LONG'  { $stat[$k].Long++ }
                }
                if ($ev -eq 'DOWN') { $stat[$k].Mv += $mv }
            }
            continue
        }
        $m2 = $reCal.Match($ln)
        if ($m2.Success) {
            $k = $m2.Groups[1].Value
            if ($stat.ContainsKey($k)) { $stat[$k].CalMin = [int]$m2.Groups[2].Value }
        }
    }
    return $stat
}

function Show-Report {
    param($Stat, [string]$Source)

    Write-Host ""
    Write-Host "================ 五位键盘验证报告 ================" -ForegroundColor Cyan
    Write-Host "数据来源: $Source"
    Write-Host ""
    Write-Host ("  {0,-4} {1,6} {2,6} {3,6} {4,6}  {5}" -f '键', 'DOWN', 'CLICK', 'LONG', 'UP', '触发读数(mV)')
    $seen = 0
    foreach ($k in @('1','2','3','4','OK')) {
        $s = $Stat[$k]
        if ($s.Down -gt 0) { $seen++ }
        $mvTxt = if ($s.Mv.Count -gt 0) { ($s.Mv -join ', ') } else { '-' }
        if ($s.CalMin -ne $null) { $mvTxt += "   (标定最低 $($s.CalMin))" }
        $color = if ($s.Down -gt 0) { 'Green' } else { 'DarkGray' }
        Write-Host ("  {0,-4} {1,6} {2,6} {3,6} {4,6}  {5}" -f $k, $s.Down, $s.Click, $s.Long, $s.Up, $mvTxt) -ForegroundColor $color
    }
    Write-Host ""

    $okStat = $Stat['OK']
    $dirsOk = (@('1','2','3','4') | Where-Object { $Stat[$_].Down -gt 0 }).Count

    if ($seen -eq 0) {
        Write-Host "[FAIL] 一个按键事件都没有：检查 IO10 接线 / 固件是否已烧录 / 端口是否选对" -ForegroundColor Red
        return 1
    }
    if ($dirsOk -lt 4) {
        Write-Host "[WARN] 只抓到 $dirsOk/4 个方向键，可能没按全或者某个键接触不良" -ForegroundColor Yellow
    }
    if ($okStat.Down -eq 0) {
        Write-Host "[FAIL] ★OK 键没有任何事件★ —— 这正是本次要修的问题，仍然存在" -ForegroundColor Red
        Write-Host "       看 keyscan 输出里按 OK 时 IO10 的电压：若完全不动，多半是接线/按键硬件" -ForegroundColor Red
        return 1
    }

    $okMv = if ($okStat.Mv.Count -gt 0) { ($okStat.Mv | Measure-Object -Minimum).Minimum } else { $null }
    if ($okMv -ne $null -and $okMv -le 300) {
        Write-Host "[PASS] 五个键全部有事件；OK 触发读数 ${okMv}mV → 命中低压档（0~300mV）" -ForegroundColor Green
        Write-Host "       → 菜单里按 OK 应该能确认/进入了" -ForegroundColor Green
        return 0
    }
    Write-Host "[PASS] OK 有事件；触发读数 $(if ($okMv -ne $null) { "${okMv}mV" } else { '(未记录)' }) —— 未落在 ≤300mV 低压档，走的是高压残余档" -ForegroundColor Yellow
    Write-Host "       如果按 OK 有反应就没问题；若偶发误触发，把 ADKEY_OK_MIN_MV 调低" -ForegroundColor Yellow
    return 0
}

# 回放存好的日志
if ($Replay) {
    if (-not (Test-Path $Replay)) { Write-Host "[FAIL] 日志不存在: $Replay" -ForegroundColor Red; exit 1 }
    $lines = Get-Content $Replay
    $stat  = Parse-AdkeyLog -Lines $lines
    exit (Show-Report -Stat $stat -Source $Replay)
}

# 没给端口就自己挑
if (-not $Port) {
    $cand = @([System.IO.Ports.SerialPort]::GetPortNames()) | Where-Object { $BT -notcontains $_ }
    if (-not $cand) {
        Write-Host "[FAIL] 没找到开发板串口（当前: " -ForegroundColor Red -NoNewline
        Write-Host ((@([System.IO.Ports.SerialPort]::GetPortNames()) -join ', ') + "）") -ForegroundColor Red
        Write-Host "       请插上开发板；或用 -Port COMx 指定" -ForegroundColor Red
        exit 2
    }
    $Port = ($cand | Sort-Object)[0]
    Write-Host "[OK]   自动选中串口: $Port" -ForegroundColor Green
}

Write-Host ""
Write-Host "接下来 $Seconds 秒内，请【依次按下】五个键，每个键按住约 1 秒：" -ForegroundColor Cyan
Write-Host "      丝印 1 → 2 → 3 → 4 → OK" -ForegroundColor Cyan
Write-Host "      （板子会自动复位一次，属正常）" -ForegroundColor DarkGray
Write-Host ""
for ($i = 5; $i -ge 1; $i--) { Write-Host "  $i ..." -NoNewline; Start-Sleep -Seconds 1 }
Write-Host "`n  开始采样！" -ForegroundColor Yellow

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 200
$sp.DtrEnable = $false
$sp.RtsEnable = $false
try { $sp.Open() } catch {
    Write-Host "[FAIL] 打不开 $Port : $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "       是不是别的串口工具占着？先关掉再试" -ForegroundColor Red
    exit 2
}

$sb = New-Object System.Text.StringBuilder
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try {
        $n = $sp.BytesToRead
        if ($n -gt 0) {
            $buf = New-Object byte[] $n
            $r = $sp.Read($buf, 0, $n)
            [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r))
        } else { Start-Sleep -Milliseconds 60 }
    } catch { Start-Sleep -Milliseconds 60 }
}
$sp.Close(); $sp.Dispose()

$raw = $sb.ToString()
$outFile = Join-Path $logDir ("adkey-verify-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
if (-not $NoLog) { $raw | Out-File $outFile -Encoding UTF8; Write-Host "原始日志: $outFile" }

$stat = Parse-AdkeyLog -Lines ($raw -split "`r?`n")
exit (Show-Report -Stat $stat -Source "串口 $Port（$Seconds 秒）")
