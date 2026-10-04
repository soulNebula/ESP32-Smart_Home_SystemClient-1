# 模块：
#   idf_pio.ps1 的启动壳。命令行直接调它，它转手给 idf_pio.ps1 去干活。
#   为什么要多这一层：idf_pio.ps1 里有中文，而 PowerShell 5.1 读到
#   没有 BOM 的 .ps1 会按本机编码（这里是 GBK）解码，中文就乱码、脚本报错。
#   这个壳在转手之前会先检查并给 idf_pio.ps1 补上 BOM。
#   用法：
#       powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task build
#       powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task flash -Port COM31
#       powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task erase-flash,flash -Port COM31
#   多余的参数会原样转给 idf_pio.ps1。
#
# 功能：
#   转手给 IDF 脚本
#   给中文脚本补 BOM

[CmdletBinding()]
param(
    [string[]]$Task = @('build'),
    [string]$Port = '',
    [switch]$Reinstall
)

$ErrorActionPreference = 'Stop'
$scriptDir = $PSScriptRoot
$target    = Join-Path $scriptDir 'idf_pio.ps1'

if (-not (Test-Path $target)) {
    Write-Host "[FAIL] not found: $target" -ForegroundColor Red
    exit 1
}

# 功能：缺 BOM 就补上
$bytes = [System.IO.File]::ReadAllBytes($target)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
if (-not $hasBom) {
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    [System.IO.File]::WriteAllText($target, $text, (New-Object System.Text.UTF8Encoding($true)))
    Write-Host "[fix]  added UTF-8 BOM to idf_pio.ps1" -ForegroundColor Yellow
}

# 功能：把参数转过去
$fwd = @{ Task = $Task }
if ($Port)       { $fwd['Port'] = $Port }
if ($Reinstall)  { $fwd['Reinstall'] = $true }

& $target @fwd
exit $LASTEXITCODE
