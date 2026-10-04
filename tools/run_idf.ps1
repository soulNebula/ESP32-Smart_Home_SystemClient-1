# =============================================================================
#  run_idf.ps1 -- ASCII-only launcher for tools\idf_pio.ps1
# =============================================================================
#  WHY THIS EXISTS
#  ---------------------------------------------------------------------------
#  This session's shell is Windows PowerShell 5.1, which decodes a .ps1 file
#  using the ANSI code page (GBK here) UNLESS the file starts with a UTF-8 BOM.
#  tools\idf_pio.ps1 contains Chinese text, so if its BOM is ever lost (e.g. a
#  tool rewrites the file as plain UTF-8) the script fails with
#  "The string is missing the terminator" / mojibake.
#
#  This wrapper is pure ASCII, so it always runs, and it re-adds the BOM to
#  idf_pio.ps1 before invoking it. Use it instead of calling idf_pio.ps1 direct:
#
#      powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task build
#      powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task flash -Port COM31
#      powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task erase-flash,flash -Port COM31
#
#  Extra args are forwarded verbatim to idf_pio.ps1.
# =============================================================================
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

# --- ensure UTF-8 BOM on the Chinese-containing script -----------------------
$bytes = [System.IO.File]::ReadAllBytes($target)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
if (-not $hasBom) {
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    [System.IO.File]::WriteAllText($target, $text, (New-Object System.Text.UTF8Encoding($true)))
    Write-Host "[fix]  added UTF-8 BOM to idf_pio.ps1" -ForegroundColor Yellow
}

# --- forward to the real script ---------------------------------------------
$fwd = @{ Task = $Task }
if ($Port)       { $fwd['Port'] = $Port }
if ($Reinstall)  { $fwd['Reinstall'] = $true }

& $target @fwd
exit $LASTEXITCODE
