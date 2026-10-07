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

# 找一个 gcc 来用
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

# 控制台转成中文能看
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
& "$env:SystemRoot\System32\chcp.com" 65001 | Out-Null

# gcc 目录要先进 PATH
$ccDir = Split-Path -Parent $Cc
if ($ccDir -and (Test-Path $ccDir)) {
    $env:PATH = $ccDir + ';' + $env:PATH
}

# 在暂存目录里编译
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

# 跑测试
& $exe
$rc = $LASTEXITCODE
Write-Host ""
if ($rc -eq 0) { Write-Host '[PASS] adkey logic test: all cases passed' -ForegroundColor Green }
else           { Write-Host "[FAIL] adkey logic test: exit $rc" -ForegroundColor Red }
exit $rc
