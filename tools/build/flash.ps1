[CmdletBinding()]
param(
    [string] $Port     = 'COM31',
    [int]    $Baud     = 460800,
    [string] $BuildDir = (Join-Path $PSScriptRoot '..\..\build'),
    [switch] $Erase
)

$ErrorActionPreference = 'Continue'

function Say([string]$msg, [string]$color = 'Gray') {
    Write-Host $msg -ForegroundColor $color
}
function Strip([string]$s) { return ($s -replace "\x1b\[[0-9;]*m", "") }

Say ""
Say "=== flash.ps1 : uncompressed flash, auto-retry with chip erase ===" Cyan
Say "    port      : $Port"
Say "    baud      : $Baud"
Say "    build dir : $BuildDir"
Say "    force erase: $(if ($Erase) { 'YES' } else { 'no (auto-retry if verify fails)' })"
Say ""

# 先确认镜像都在
$flashArgs = Join-Path $BuildDir 'flash_args'
if (-not (Test-Path $flashArgs)) {
    Say "[FAIL] $flashArgs not found." Red
    Say "       Build first:  powershell -ExecutionPolicy Bypass -File tools\build\build.ps1 -Task build" Red
    exit 2
}
$argsTxt = [System.IO.File]::ReadAllText($flashArgs, [System.Text.Encoding]::UTF8)
$images  = ([regex]::Matches($argsTxt, '(?m)^\s*(0x[0-9a-fA-F]+)\s+(\S+)\s*$'))
Say "  images to write:"
foreach ($m in $images) {
    $p      = Join-Path $BuildDir $m.Groups[2].Value
    $exists = Test-Path $p
    $len    = if ($exists) { (Get-Item $p).Length } else { 0 }
    Say ("    {0,-10} {1,-34} {2,10} B  {3}" -f `
            $m.Groups[1].Value, $m.Groups[2].Value, $len, $(if ($exists) { 'OK' } else { 'MISSING' })) `
        $(if ($exists) { 'Gray' } else { 'Red' })
    if (-not $exists) { Say "[FAIL] missing image, aborting." Red; exit 2 }
}
Say ""

# 找带 esptool 的 Python
$pyCandidates = @(
    'E:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe',
    (Join-Path $env:IDF_TOOLS_PATH 'python_env\idf5.4_py3.11_env\Scripts\python.exe')
)
$py = $null
foreach ($c in $pyCandidates) { if ($c -and (Test-Path $c)) { $py = $c; break } }
if (-not $py) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $py = $cmd.Source }
}
if (-not $py) {
    Say "[FAIL] could not find a python with esptool installed." Red
    exit 2
}
Say "  python    : $py"
Say ""

# 只读端口列表
$busy = @(Get-Process -Name python -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
    Say "[WARN] other python processes are running - is something else flashing?" Yellow
    foreach ($b in $busy) { Say ("        PID {0} started {1}" -f $b.Id, $b.StartTime) Yellow }
    Say "        aborting to avoid a corrupted write." Yellow
    exit 3
}
$ports = [System.IO.Ports.SerialPort]::GetPortNames()
Say ("  serial ports: {0}" -f ($ports -join ', '))
if ($ports -notcontains $Port) {
    Say "[FAIL] $Port is not present." Red
    Say "       Unplug/replug the board, then check Device Manager." Red
    exit 4
}
Say ""

# 写入，失败就擦片重来
$attempts = if ($Erase) { @($true) } else { @($false, $true) }
$code = 0; $hashN = 0; $fatal = $false
$out = @(); $usedErase = $false; $ok = $false

Push-Location $BuildDir
try {
    for ($i = 0; $i -lt $attempts.Count; $i++) {
        $doErase = $attempts[$i]

        if ($doErase) {
            Say "=== erase_flash (full chip erase) ===" Cyan
            $er = & $py -m esptool --chip esp32s3 -p $Port -b $Baud `
                        --before default_reset --after no_reset erase_flash 2>&1
            $er | ForEach-Object { Write-Host ("  " + (Strip $_)) }
            if ($LASTEXITCODE -ne 0) {
                Say "[FAIL] erase_flash failed." Red
                $code = $LASTEXITCODE
                break
            }
            $usedErase = $true
            Say ""
        }

        Say ("=== write_flash --no-compress @flash_args   (attempt {0}/{1}{2}) ===" -f `
                ($i + 1), $attempts.Count, $(if ($doErase) { ', after chip erase' } else { '' })) Cyan
        $out = & $py -m esptool --chip esp32s3 -p $Port -b $Baud `
                    --before default_reset --after hard_reset `
                    write_flash --no-compress '@flash_args' 2>&1
        $code = $LASTEXITCODE
        $out | ForEach-Object { Write-Host ("  " + (Strip $_)) }

        $text  = ($out | Out-String)
        $hashN = ([regex]::Matches($text, 'Hash of data verified')).Count
        $fatal = ($text -match 'A fatal error occurred') -or ($text -match 'MD5 of file does not match')

        if (($code -eq 0) -and ($hashN -ge 5) -and (-not $fatal)) { $ok = $true; break }

        if (-not $doErase) {
            Say ""
            Say "[WARN] write did NOT verify without a chip erase." Yellow
            Say "[WARN] This is the known failure mode of this board (see header)." Yellow
            Say "[WARN] Retrying once WITH a full chip erase ..." Yellow
            Say ""
        }
    }
}
finally {
    Pop-Location
}

# 最后给个结论
Say ""
if ($ok) {
    Say ("=== FLASH OK : {0}/5 images verified{1} ===" -f `
            $hashN, $(if ($usedErase) { ' (after chip erase)' } else { '' })) Green
    Say "    next:  powershell -ExecutionPolicy Bypass -File tools\monitor\monitor.ps1 -Port $Port -Seconds 45 -Reset"
    exit 0
}
Say "[FAIL] flashing did not verify." Red
Say ("       exit code                     = {0}" -f $code) Red
Say ("       'Hash of data verified' lines  = {0}  (expected 5)" -f $hashN) Red
Say ("       chip erase was used           = {0}" -f $usedErase) Red
if ($fatal -and $usedErase) {
    Say "       Still an MD5 mismatch even AFTER a chip erase.  Next things to" Red
    Say "       try, in order:" Red
    Say "         1. a different USB cable / a powered USB hub (power dips)" Red
    Say "         2. -Baud 115200" Red
    Say "         3. a different serial adapter (CH340 vs CP210x vs native USB)" Red
}
exit 6
