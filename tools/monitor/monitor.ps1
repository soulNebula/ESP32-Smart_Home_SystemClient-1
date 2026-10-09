[CmdletBinding()]
param(
    [string]$Port = '',
    [int]$Baud = 0,
    [string]$LogDir = '',
    [int]$Seconds = 0,
    [switch]$Reset,
    [string[]]$Send = @(),
    [switch]$NoEcho,
    [switch]$NoLog,
    [switch]$List
)

$ErrorActionPreference = 'Continue'

$projectDir = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $LogDir) { $LogDir = Join-Path $projectDir 'logs' }

$stateDir  = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'esp32-smarthome' } else { $env:TEMP }
$stateFile = Join-Path $stateDir 'monitor-last.json'

$script:tty = $false
try { $script:tty = -not [Console]::IsInputRedirected } catch { $script:tty = $false }

$BaudList    = @(9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)
$DefaultBaud = 115200

# Tab 补全用的候选表
$CMD_LIST  = @('help','help-voice','status','on','off','toggle','set','color',
               'open','close','auto','cfg','say','log')
$DEV_LIST  = @('led_living','led_kitchen','led_bedroom','led_bath','fan','window','door','curtain','all')
$LOG_LIST  = @('quiet','all','n','e','w','i','d','v')
$AUTO_LIST = @('on','off')
$CFG_LIST  = @('enabled','light_on_lux','light_off_lux','temp_fan_on_c','temp_fan_off_c',
               'rain_pct','fan_auto_speed','auto_light_enable','auto_temp_enable',
               'auto_rain_enable','auto_window_reopen')
$SAY_LIST  = @('led_living_on','led_living_off','led_kitchen_on','led_kitchen_off',
               'led_bedroom_on','led_bedroom_off','led_bath_on','led_bath_off',
               'led_all_on','led_all_off','fan_on','fan_off','window_open','window_close',
               'door_open','door_close','curtain_open','curtain_close',
               'query_temp','query_humi','query_light','query_all','query_status',
               'auto_on','auto_off')

function Get-Pool([string]$cmd, [int]$idx) {
    if ($idx -eq 0) { return $script:CMD_LIST }
    if ($idx -ne 1) { return @() }
    switch ($cmd) {
        'on'     { return $script:DEV_LIST }
        'off'    { return $script:DEV_LIST }
        'toggle' { return $script:DEV_LIST }
        'set'    { return $script:DEV_LIST }
        'open'   { return $script:DEV_LIST }
        'close'  { return $script:DEV_LIST }
        'color'  { return $script:DEV_LIST }
        'auto'   { return $script:AUTO_LIST }
        'log'    { return $script:LOG_LIST }
        'cfg'    { return $script:CFG_LIST }
        'say'    { return $script:SAY_LIST }
    }
    return @()
}

function Show-Cands($items) {
    $line = '  '
    foreach ($i in ($items | Sort-Object)) {
        if (($line.Length + $i.Length) -gt 74) { Write-Host $line; $line = '  ' }
        $line += $i + '  '
    }
    if ($line.Trim().Length -gt 0) { Write-Host $line }
}

function Get-Lcp($items) {
    if (-not $items -or $items.Count -eq 0) { return '' }
    $p = $items[0]
    foreach ($s in $items) {
        while ($p.Length -gt 0 -and -not $s.StartsWith($p)) { $p = $p.Substring(0, $p.Length - 1) }
    }
    return $p
}

# 唯一命中就补全
function Complete-Input([string]$text) {
    $parts   = @($text -split ' ')
    $idx     = $parts.Count - 1
    $partial = $parts[$idx]
    $pool    = @(Get-Pool $parts[0] $idx)
    if ($pool.Count -eq 0) { return $text }

    $hits = @($pool | Where-Object { $_ -like "$partial*" })
    if ($hits.Count -eq 0) { return $text }

    if ($hits.Count -eq 1) {
        $parts[$idx] = $hits[0]
        $new = ($parts -join ' ')
        Write-Host ""
        Write-Host ("  " + $new + " ") -NoNewline
        return $new
    }

    $lcp = Get-Lcp $hits
    if ($lcp.Length -gt $partial.Length) { $parts[$idx] = $lcp }
    $new = ($parts -join ' ')
    Write-Host ""
    Show-Cands $hits
    Write-Host ("  " + $new) -NoNewline
    return $new
}

$script:sp       = $null
$script:curPort  = ''
$script:curBaud  = $DefaultBaud
$script:portDesc = ''

function Say-Ok($m)  { Write-Host "  $m" -ForegroundColor Green }
function Say-Err($m) { Write-Host "  $m" -ForegroundColor Red }
function Say-Warn($m){ Write-Host "  $m" -ForegroundColor Yellow }

function Load-State {
    try {
        if (Test-Path $stateFile) { return (Get-Content $stateFile -Raw -Encoding UTF8 | ConvertFrom-Json) }
    } catch { }
    return $null
}

function Save-State {
    try {
        if (-not (Test-Path $stateDir)) { New-Item -ItemType Directory -Path $stateDir -Force | Out-Null }
        @{ port = $script:curPort; baud = $script:curBaud } | ConvertTo-Json |
            Set-Content -Path $stateFile -Encoding UTF8
    } catch { }
}

function Port-Num([string]$n) { if ($n -match '(\d+)\s*$') { return [int]$Matches[1] } return 9999 }

function Scan-Ports {
    $names = @([System.IO.Ports.SerialPort]::GetPortNames())
    $friendly = @{}
    try {
        Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
            Where-Object { $_.Name -match '\((COM\d+)\)' } |
            ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $friendly[$Matches[1]] = $_.Name } }
    } catch { }

    $out = @()
    foreach ($n in ($names | Sort-Object { Port-Num $_ })) {
        $d = if ($friendly.ContainsKey($n)) { $friendly[$n] -replace '\s*\(COM\d+\)\s*$', '' } else { '?' }
        $out += [pscustomobject]@{ Port = $n; Desc = $d }
    }
    return ,$out
}

function Show-Ports($ports) {
    Write-Host ""
    if ($ports.Count -eq 0) { Say-Err "没扫到串口"; return }
    Write-Host " 扫描到 $($ports.Count) 个串口:"
    for ($i = 0; $i -lt $ports.Count; $i++) {
        Write-Host ("   {0,2}  {1,-7} {2}" -f ($i + 1), $ports[$i].Port, $ports[$i].Desc)
    }
    Write-Host ""
}

function Pick-Port {
    $ports = Scan-Ports
    Show-Ports $ports
    if ($ports.Count -eq 0) { return }
    $a = Read-Host " 序号 [1-$($ports.Count)]"
    if ([string]::IsNullOrWhiteSpace($a)) { return }
    $i = 0
    if ([int]::TryParse($a.Trim(), [ref]$i) -and $i -ge 1 -and $i -le $ports.Count) {
        $script:curPort  = $ports[$i - 1].Port
        $script:portDesc = $ports[$i - 1].Desc
        Save-State
    }
}

function Pick-Baud {
    Write-Host ""
    Write-Host " 波特率:"
    for ($r = 0; $r -lt 4; $r++) {
        Write-Host ("   {0}  {1,-8} {2}  {3}" -f ($r + 1), $BaudList[$r], ($r + 5), $BaudList[$r + 4])
    }
    Write-Host ""
    $a = Read-Host " 序号 [1-8] 或直接输数字"
    if ([string]::IsNullOrWhiteSpace($a)) { return }
    $i = 0
    if ([int]::TryParse($a, [ref]$i)) {
        if ($i -ge 1 -and $i -le 8) { $script:curBaud = $BaudList[$i - 1] }
        elseif ($i -gt 0)           { $script:curBaud = $i }
        Save-State
    }
}

function Close-Port {
    if ($script:sp) {
        try { if ($script:sp.IsOpen) { $script:sp.Close() }; $script:sp.Dispose() } catch { }
        $script:sp = $null
    }
}

function Open-Port {
    if (-not $script:curPort) { Say-Err "没选端口"; return $false }
    Close-Port
    $s = New-Object System.IO.Ports.SerialPort
    $s.PortName = $script:curPort; $s.BaudRate = $script:curBaud
    $s.Parity = [System.IO.Ports.Parity]::None; $s.DataBits = 8
    $s.StopBits = [System.IO.Ports.StopBits]::One
    $s.Handshake = [System.IO.Ports.Handshake]::None
    $s.ReadTimeout = 30; $s.WriteTimeout = 1000
    $s.Encoding = [System.Text.Encoding]::UTF8
    try {
        $s.Open()
        $script:sp = $s
        Save-State
        Say-Ok "已连接 $($script:curPort) @ $($script:curBaud)"
        return $true
    } catch {
        Say-Err "打开失败: $($_.Exception.Message)"
        try { $s.Dispose() } catch { }
        return $false
    }
}

function Show-Menu {
    if ($script:tty) { Clear-Host }
    Write-Host ""
    Write-Host "=============== 串口终端 ===============" -ForegroundColor Cyan
    Write-Host ("  端口    " + $(if ($script:curPort) { $script:curPort } else { '(未选)' }))
    Write-Host ("  设备    " + $(if ($script:portDesc) { $script:portDesc } else { '-' }))
    Write-Host ("  波特率  " + $script:curBaud)
    Write-Host "---------------------------------------"
    Write-Host "   1  连接"
    Write-Host "   2  选择端口"
    Write-Host "   3  设置波特率"
    Write-Host "   0  退出"
    Write-Host "=======================================" -ForegroundColor Cyan
}

# 真返回就回菜单
function Run-Monitor {
    $logWriter = $null; $logPath = $null
    if (-not $NoLog) {
        if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }
        $logPath = Join-Path $LogDir ("monitor-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        $logWriter = New-Object System.IO.StreamWriter($logPath, $false, (New-Object System.Text.UTF8Encoding($false)))
        $logWriter.AutoFlush = $true
    }

    if ($Reset) {
        try {
            $script:sp.DtrEnable = $false
            $script:sp.RtsEnable = $true; Start-Sleep -Milliseconds 150; $script:sp.RtsEnable = $false
        } catch { }
    }

    $decoder = [System.Text.Encoding]::UTF8.GetDecoder()
    $byteBuf = New-Object byte[] 8192
    $charBuf = New-Object char[] 8192
    $partial = ''; $typed = ''; $filter = $false
    $pending = New-Object System.Collections.ArrayList
    $start = Get-Date
    $backToMenu = $false

    Write-Host ""
    $hint = {
        Write-Host ""
        Write-Host "  $($script:curPort) @ $($script:curBaud)" -ForegroundColor Cyan
        Write-Host "  status | on|off <dev> | set <dev> <0-100> | say <cmd> | log quiet|all" -ForegroundColor Gray
        Write-Host "  Tab 补全（输入为空时按 Tab 也会列出候选）    ? 重显本提示    help 看全部命令" -ForegroundColor DarkGray
        Write-Host "  Ctrl+P 菜单    Ctrl+L 过滤日志    Ctrl+C 退出" -ForegroundColor DarkGray
        if ($logPath) { Write-Host "  日志 $logPath" -ForegroundColor DarkGray }
        Write-Host ("  " + ("-" * 68)) -ForegroundColor DarkGray
    }
    & $hint

    $emit = {
        param($line)
        if ($NoEcho) { return }
        $c = 'Gray'
        if     ($line -match '^\s*E \(') { $c = 'Red' }
        elseif ($line -match '^\s*W \(') { $c = 'Yellow' }
        elseif ($line -match '^\s*D \(') { $c = 'DarkGray' }
        elseif ($line -match 'ESP-ROM|boot:|rst:0x') { $c = 'DarkCyan' }
        elseif ($line -match 'got ip|connected|MQTT|topics:|system ready|board init|ready') { $c = 'Green' }
        Write-Host $line -ForegroundColor $c
    }

    if ($Send.Count -gt 0) {
        Start-Sleep -Milliseconds 600
        foreach ($c in $Send) {
            try { $script:sp.Write($c + "`r`n") } catch { }
            Start-Sleep -Milliseconds 800
        }
    }

    try {
        while (-not $backToMenu) {
            if ($Seconds -gt 0 -and ((Get-Date) - $start).TotalSeconds -ge $Seconds) { break }

            $got = $false
            if (-not ($script:sp -and $script:sp.IsOpen)) {
                Start-Sleep -Milliseconds 200
                continue
            }
            try {
                $n = $script:sp.Read($byteBuf, 0, $byteBuf.Length)
                if ($n -gt 0) {
                    $cn = $decoder.GetChars($byteBuf, 0, $n, $charBuf, 0)
                    $partial += [System.String]::new($charBuf, 0, $cn)
                    while ($true) {
                        $k = $partial.IndexOf("`n")
                        if ($k -lt 0) { break }
                        $line = $partial.Substring(0, $k).TrimEnd("`r")
                        $partial = $partial.Substring($k + 1)
                        if ($logWriter) { $logWriter.WriteLine($line) }
                        if ($filter -and $line -match '^\s*[IWEVD] \(\d+\)') { continue }
                        if ($typed.Length -gt 0) { [void]$pending.Add($line) } else { & $emit $line }
                    }
                    $got = $true
                }
            } catch [TimeoutException] {
            } catch {
                Say-Err "读取失败: $($_.Exception.Message)"
                Close-Port
                break
            }

            if ($script:tty) {
                try {
                    while ([Console]::KeyAvailable) {
                        $key = [Console]::ReadKey($true)
                        if ($key.Key -eq [ConsoleKey]::Enter) {
                            Write-Host ""
                            $cmd = $typed.Trim()
                            $typed = ''
                            foreach ($l in $pending) { & $emit $l }
                            $pending.Clear()

                            if ($cmd -eq '?') {
                                & $hint
                            }
                            elseif ($cmd) {
                                Write-Host ("  > " + $cmd) -ForegroundColor Cyan
                                if ($logWriter) { $logWriter.WriteLine(">>> " + $cmd) }
                                try { $script:sp.Write($cmd + "`r`n") } catch { }
                            }
                        }
                        elseif ($key.Key -eq [ConsoleKey]::P -and (($key.Modifiers -band [ConsoleModifiers]::Control) -eq [ConsoleModifiers]::Control)) {
                            Write-Host ""
                            $backToMenu = $true
                            break
                        }
                        elseif ($key.Key -eq [ConsoleKey]::L -and (($key.Modifiers -band [ConsoleModifiers]::Control) -eq [ConsoleModifiers]::Control)) {
                            $filter = -not $filter
                            Say-Warn $(if ($filter) { "日志已过滤（Ctrl+L 恢复）" } else { "日志已恢复" })
                        }
                        elseif ($key.Key -eq [ConsoleKey]::Backspace) {
                            if ($typed.Length -gt 0) { $typed = $typed.Substring(0, $typed.Length - 1); Write-Host "`b `b" -NoNewline }
                        }
                        elseif ($key.Key -eq [ConsoleKey]::Tab) {
                            $typed = Complete-Input $typed
                        }
                        elseif ($key.KeyChar -and [int]$key.KeyChar -ge 32 -and [int]$key.KeyChar -ne 127) {
                            $typed += $key.KeyChar
                            Write-Host $key.KeyChar -NoNewline
                        }
                    }
                } catch { $script:tty = $false }
            }

            if (-not $got) { Start-Sleep -Milliseconds 5 }
        }
    } finally {
        if ($logWriter) { $logWriter.Close() }
    }
    return $backToMenu
}

# 主流程从这儿走
$st = Load-State
if ($st) {
    if ($st.port) { $script:curPort = [string]$st.port }
    if ($st.baud) { $script:curBaud = [int]$st.baud }
}

if ($List) {
    Show-Ports (Scan-Ports)
    exit 0
}

# 指定端口就直接连
if ($Port) {
    $script:curPort = $Port
    if ($Baud -gt 0) { $script:curBaud = $Baud }
    $hit = (Scan-Ports) | Where-Object { $_.Port -eq $Port }
    if ($hit) { $script:portDesc = $hit.Desc }
    else { Say-Err "串口 $Port 不存在"; exit 1 }
    if (-not (Open-Port)) { exit 1 }
    [void](Run-Monitor)
    Close-Port
    exit 0
}

# 无人值守用上次端口
if (-not $script:tty) {
    if (-not $script:curPort) {
        $p = Scan-Ports
        if ($p.Count -eq 0) { Say-Err "没扫到串口"; exit 1 }
        $script:curPort = $p[0].Port
    }
    if ($Baud -gt 0) { $script:curBaud = $Baud }
    if (-not (Open-Port)) { exit 1 }
    [void](Run-Monitor)
    Close-Port
    exit 0
}

while ($true) {
    Show-Menu
    $c = Read-Host " 请输入"
    if ($null -eq $c) { continue }
    $c = $c.Trim()

    if ($c -eq '1') {
        if (Open-Port) { [void](Run-Monitor); Close-Port }
    }
    elseif ($c -eq '2') { Pick-Port }
    elseif ($c -eq '3') { Pick-Baud }
    elseif ($c -eq '0') { Close-Port; exit 0 }
}
