[CmdletBinding()]
param(
    [string[]]$Task = @('build'),
    [string]$Port = '',
    [switch]$Reinstall
)

$ErrorActionPreference = 'Stop'
# idf_pio.ps1 与本脚本同在 tools\build\ 下
$scriptDir = $PSScriptRoot
$target    = Join-Path $scriptDir 'idf_pio.ps1'

if (-not (Test-Path $target)) {
    Write-Host "[FAIL] not found: $target" -ForegroundColor Red
    exit 1
}

# 缺 BOM 就补上
$bytes = [System.IO.File]::ReadAllBytes($target)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
if (-not $hasBom) {
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    [System.IO.File]::WriteAllText($target, $text, (New-Object System.Text.UTF8Encoding($true)))
    Write-Host "[fix]  added UTF-8 BOM to idf_pio.ps1" -ForegroundColor Yellow
}

# 把参数转过去
$fwd = @{ Task = $Task }
if ($Port)       { $fwd['Port'] = $Port }
if ($Reinstall)  { $fwd['Reinstall'] = $true }

& $target @fwd
exit $LASTEXITCODE
