# =============================================================================
#  run_adkey_test.ps1 -- host-side unit test for the AD-key match logic
# =============================================================================
#  Compiles tools\adkey_test.c with a host C compiler (MinGW gcc) and runs it.
#  No ESP-IDF and no board needed: the test includes the SAME header the
#  firmware uses (components\BSP\ADKEY\adkey_logic.h), so it exercises the
#  shipped thresholds.
#
#      powershell -ExecutionPolicy Bypass -File tools\run_adkey_test.ps1
#
#  Exit codes: 0 = all cases pass, 1 = compile/run failure, 2 = no compiler.
#
#  !! THIS FILE MUST STAY PURE ASCII !!
#  Windows PowerShell 5.1 decodes a .ps1 with the ANSI code page (GBK here)
#  unless the file starts with a UTF-8 BOM. Any non-ASCII byte in this file
#  (Chinese comment, star, arrow...) therefore turns into mojibake and breaks
#  the script. Keep it ASCII; the C test prints Chinese, which is fine because
#  the console is switched to UTF-8 below.
#
#  MinGW gcc also cannot handle the Chinese path of this project, so the test
#  source and the header under test are staged into an ASCII temp directory
#  before compiling.
# =============================================================================
[CmdletBinding()]
param(
    [string]$Cc = ''
)

$ErrorActionPreference = 'Stop'

$projectDir = (Get-Item (Split-Path -Parent $PSScriptRoot)).FullName
$src        = Join-Path $PSScriptRoot 'adkey_test.c'
$hdr        = Join-Path $projectDir 'components\BSP\ADKEY\adkey_logic.h'
$stage      = Join-Path $env:TEMP 'adkey_test_build'
$exe        = Join-Path $stage 'adkey_test.exe'

Write-Host "project  : $projectDir"
Write-Host "stage    : $stage"

try {
    if (-not (Test-Path $src)) { Write-Host "[FAIL] missing $src" -ForegroundColor Red; exit 1 }
    if (-not (Test-Path $hdr)) { Write-Host "[FAIL] missing $hdr" -ForegroundColor Red; exit 1 }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item $src (Join-Path $stage 'adkey_test.c') -Force
    Copy-Item $hdr (Join-Path $stage 'adkey_logic.h') -Force
} catch {
    Write-Host "[FAIL] staging failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "       $($_.InvocationInfo.PositionMessage)" -ForegroundColor Red
    exit 1
}

# --- locate a host compiler --------------------------------------------------
if (-not $Cc) {
    $cands = @(
        'C:\msys64\mingw64\bin\gcc.exe',
        'C:\msys64\ucrt64\bin\gcc.exe',
        'C:\MinGW\bin\gcc.exe'
    )
    foreach ($c in $cands) { if (Test-Path $c) { $Cc = $c; break } }
    if (-not $Cc) {
        $g = Get-Command gcc.exe -ErrorAction SilentlyContinue
        if ($g) { $Cc = $g.Source }
    }
}
if (-not $Cc) {
    Write-Host '[FAIL] no host C compiler found (msys64/mingw64 gcc, or gcc in PATH)' -ForegroundColor Red
    exit 2
}
if (-not (Test-Path $Cc)) {
    Write-Host "[FAIL] compiler not found: $Cc" -ForegroundColor Red
    exit 2
}

# --- console -> UTF-8 so the Chinese test output is readable -----------------
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
& "$env:SystemRoot\System32\chcp.com" 65001 | Out-Null

# --- make the toolchain self-consistent --------------------------------------
# MSYS2's gcc MUST find cc1.exe / as / ld through PATH; calling it by absolute
# path alone makes it exit 1 with no diagnostics at all. So put its bin dir in
# PATH first.
$ccDir = Split-Path -Parent $Cc
if ($ccDir -and (Test-Path $ccDir)) {
    $env:PATH = $ccDir + ';' + $env:PATH
}

# --- compile (inside the ASCII staging dir) ----------------------------------
$ccOut = & $Cc -std=c11 -Wall -Wextra -O1 -I $stage -o $exe (Join-Path $stage 'adkey_test.c') 2>&1
$ccRc  = $LASTEXITCODE
if ($ccOut) { $ccOut | ForEach-Object { Write-Host "  $_" } }
if ($ccRc -ne 0) {
    Write-Host "[FAIL] compile failed (gcc exit $ccRc)" -ForegroundColor Red
    exit 1
}
Write-Host "compiler : $Cc"
Write-Host "binary   : $exe"
Write-Host ""

# --- run --------------------------------------------------------------------
& $exe
$rc = $LASTEXITCODE
Write-Host ""
if ($rc -eq 0) { Write-Host '[PASS] adkey logic test: all cases passed' -ForegroundColor Green }
else           { Write-Host "[FAIL] adkey logic test: exit $rc" -ForegroundColor Red }
exit $rc
