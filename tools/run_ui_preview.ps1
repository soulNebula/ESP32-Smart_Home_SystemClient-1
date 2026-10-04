# =============================================================================
#  run_ui_preview.ps1 -- host-side 128x64 layout preview for the OLED UI
# =============================================================================
#  Compiles tools\ui_layout_preview.cpp with MinGW g++ plus the project's own
#  u8g2 sources, and renders the home page layout into a RAM buffer, printing
#  an ASCII picture plus a per-element ink bounding box / overlap report.
#
#  No board, no flashing: use it to check a layout change before flashing.
#  The preview includes the SAME config.h (row baselines) and the SAME fonts
#  the firmware uses.
#
#      powershell -ExecutionPolicy Bypass -File tools\run_ui_preview.ps1
#
#  !! KEEP THIS FILE PURE ASCII !!  (PowerShell 5.1 + no BOM = GBK mojibake)
#  MinGW cannot handle the Chinese project path, so everything is staged into
#  an ASCII temp directory first.
# =============================================================================
[CmdletBinding()]
param(
    [string]$Cxx = '',
    [switch]$KeepObjects
)

$ErrorActionPreference = 'Stop'

$projectDir = (Get-Item (Split-Path -Parent $PSScriptRoot)).FullName
$src        = Join-Path $PSScriptRoot 'ui_layout_preview.cpp'
$u8g2Dir    = Join-Path $projectDir 'components\u8g2\csrc'
$cfgFile    = Join-Path $projectDir 'components\astra_ui\astra\config\config.h'

$stage = Join-Path $env:TEMP 'ui_preview_build'
$exe   = Join-Path $stage 'ui_preview.exe'

foreach ($p in @($src, $u8g2Dir, $cfgFile)) {
    if (-not (Test-Path $p)) { Write-Host "[FAIL] missing $p" -ForegroundColor Red; exit 1 }
}

# --- locate compilers --------------------------------------------------------
function Find-Tool([string]$name, [string]$explicit) {
    if ($explicit -and (Test-Path $explicit)) { return (Get-Item $explicit).FullName }
    $cands = @(
        "C:\msys64\mingw64\bin\$name",
        "C:\msys64\ucrt64\bin\$name",
        "C:\MinGW\bin\$name"
    )
    foreach ($c in $cands) { if (Test-Path $c) { return $c } }
    $g = Get-Command $name -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    return $null
}
$gcc = Find-Tool 'gcc.exe'  ''
$gpp = Find-Tool 'g++.exe'  $Cxx
if (-not $gcc -or -not $gpp) { Write-Host '[FAIL] need MinGW gcc.exe and g++.exe' -ForegroundColor Red; exit 2 }

# MSYS2 gcc/g++ must find cc1/as/ld through PATH
foreach ($d in @((Split-Path -Parent $gcc), (Split-Path -Parent $gpp))) {
    if ($d -and (Test-Path $d)) { $env:PATH = $d + ';' + $env:PATH }
}

# --- stage sources into an ASCII dir ----------------------------------------
Write-Host "staging  : $stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'u8g2') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'cfg')  | Out-Null
Copy-Item $src (Join-Path $stage 'ui_layout_preview.cpp') -Force
Copy-Item $cfgFile (Join-Path $stage 'cfg\config.h') -Force
Copy-Item (Join-Path $u8g2Dir '*.c') (Join-Path $stage 'u8g2') -Force
Copy-Item (Join-Path $u8g2Dir '*.h') (Join-Path $stage 'u8g2') -Force

# --- compile u8g2 as C ------------------------------------------------------
# ★ -DU8G2_USE_LARGE_FONTS 必须加：
#   u8g2.h only defines it for unix/arm/ESP8266/ESP_PLATFORM/... platforms, and
#   the ESP32 build gets it via ESP_PLATFORM. On Windows/MinGW none of those
#   macros exist, so without this define all the CJK (wqy12) fonts are compiled
#   OUT and the link fails with "undefined reference to u8g2_font_wqy12_t_gb2312".
$defs   = @('-DU8G2_USE_LARGE_FONTS')
$cfiles = @(Get-ChildItem (Join-Path $stage 'u8g2') -Filter '*.c' | ForEach-Object { $_.FullName })
Write-Host "u8g2 sources : $($cfiles.Count) files"
$objs = @()
foreach ($f in $cfiles) {
    $o = [System.IO.Path]::ChangeExtension($f, '.o')
    & $gcc -c -O1 -w @defs -I (Join-Path $stage 'u8g2') -o $o $f
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] gcc failed on $f" -ForegroundColor Red; exit 1 }
    $objs += $o
}

# --- compile + link the preview as C++ --------------------------------------
& $gpp -std=c++17 -O1 -w @defs -I (Join-Path $stage 'u8g2') -I (Join-Path $stage 'cfg') `
       -o $exe (Join-Path $stage 'ui_layout_preview.cpp') @objs
if ($LASTEXITCODE -ne 0) { Write-Host '[FAIL] g++ link failed' -ForegroundColor Red; exit 1 }

# --- console -> UTF-8 so the ASCII art borders show up ----------------------
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
& "$env:SystemRoot\System32\chcp.com" 65001 | Out-Null

& $exe
$rc = $LASTEXITCODE
if (-not $KeepObjects) { Get-ChildItem (Join-Path $stage 'u8g2') -Filter '*.o' | Remove-Item -Force -ErrorAction SilentlyContinue }
exit $rc
