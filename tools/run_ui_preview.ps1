# 模块：
#   在电脑上预览 OLED 界面。不接板子、不烧录，改完排版先看效果再刷固件。
#   它用 MinGW 的 g++ 把 ui_layout_preview.cpp 和本工程的 u8g2 源码一起编，
#   把首页排版画到内存里，然后打印一张字符画，外加每个元素的占位框和
#   重叠报告。用的是和固件同一份 config.h 和同一套字库，所以看的就是真效果。
#
#   两个环境上的坑（都踩过）：
#   1) MinGW 处理不了带中文的工程路径，所以先把源码拷到一个纯英文的临时
#      目录再编译。
#   2) 编译时必须加 -DU8G2_USE_LARGE_FONTS。u8g2.h 只在 unix/arm/ESP8266/
#      ESP_PLATFORM 这些平台下自动定义它，ESP32 靠 ESP_PLATFORM 拿到；
#      Windows + MinGW 一个都不占，不加这个宏中文（wqy12）字库会被整个
#      编掉，链接时报 undefined reference to u8g2_font_wqy12_t_gb2312。
#
#   用法：
#       powershell -ExecutionPolicy Bypass -File tools\run_ui_preview.ps1
#
# 功能：
#   暂存源码到英文目录
#   编译并链接预览
#   打印字符画

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

# 功能：找编译器
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

# 功能：编译器目录加进 PATH
foreach ($d in @((Split-Path -Parent $gcc), (Split-Path -Parent $gpp))) {
    if ($d -and (Test-Path $d)) { $env:PATH = $d + ';' + $env:PATH }
}

# 功能：源码拷到英文目录
Write-Host "staging  : $stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'u8g2') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'cfg')  | Out-Null
Copy-Item $src (Join-Path $stage 'ui_layout_preview.cpp') -Force
Copy-Item $cfgFile (Join-Path $stage 'cfg\config.h') -Force
Copy-Item (Join-Path $u8g2Dir '*.c') (Join-Path $stage 'u8g2') -Force
Copy-Item (Join-Path $u8g2Dir '*.h') (Join-Path $stage 'u8g2') -Force

# 功能：按 C 编 u8g2
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

# 功能：编成 C++ 并链接
& $gpp -std=c++17 -O1 -w @defs -I (Join-Path $stage 'u8g2') -I (Join-Path $stage 'cfg') `
       -o $exe (Join-Path $stage 'ui_layout_preview.cpp') @objs
if ($LASTEXITCODE -ne 0) { Write-Host '[FAIL] g++ link failed' -ForegroundColor Red; exit 1 }

# 功能：控制台转 UTF-8
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
& "$env:SystemRoot\System32\chcp.com" 65001 | Out-Null

& $exe
$rc = $LASTEXITCODE
if (-not $KeepObjects) { Get-ChildItem (Join-Path $stage 'u8g2') -Filter '*.o' | Remove-Item -Force -ErrorAction SilentlyContinue }
exit $rc
