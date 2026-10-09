[CmdletBinding()]
param(
    [string]$Cxx = '',
    [switch]$KeepObjects
)

$ErrorActionPreference = 'Stop'

$projectDir = (Get-Item (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))).FullName
# $src        = Join-Path $PSScriptRoot '..\ui_layout_preview.cpp'
$u8g2Dir    = Join-Path $projectDir 'components\u8g2\csrc'
$cfgFile    = Join-Path $projectDir 'components\astra_ui\astra\config\config.h'

$stage = Join-Path $env:TEMP 'ui_preview_build'
$exe   = Join-Path $stage 'ui_preview.exe'

foreach ($p in @($src, $u8g2Dir, $cfgFile)) {
    if (-not (Test-Path $p)) { Write-Host "[FAIL] missing $p" -ForegroundColor Red; exit 1 }
}

# 找编译器
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

# 编译器目录加进 PATH
foreach ($d in @((Split-Path -Parent $gcc), (Split-Path -Parent $gpp))) {
    if ($d -and (Test-Path $d)) { $env:PATH = $d + ';' + $env:PATH }
}

# 源码拷到英文目录
Write-Host "staging  : $stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'u8g2') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'cfg')  | Out-Null
Copy-Item $src (Join-Path $stage 'ui_layout_preview.cpp') -Force
Copy-Item $cfgFile (Join-Path $stage 'cfg\config.h') -Force
Copy-Item (Join-Path $u8g2Dir '*.c') (Join-Path $stage 'u8g2') -Force
Copy-Item (Join-Path $u8g2Dir '*.h') (Join-Path $stage 'u8g2') -Force

# 按 C 编 u8g2
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

# 编成 C++ 并链接
& $gpp -std=c++17 -O1 -w @defs -I (Join-Path $stage 'u8g2') -I (Join-Path $stage 'cfg') `
       -o $exe (Join-Path $stage 'ui_layout_preview.cpp') @objs
if ($LASTEXITCODE -ne 0) { Write-Host '[FAIL] g++ link failed' -ForegroundColor Red; exit 1 }

# 控制台转 UTF-8
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
& "$env:SystemRoot\System32\chcp.com" 65001 | Out-Null

& $exe
$rc = $LASTEXITCODE
if (-not $KeepObjects) { Get-ChildItem (Join-Path $stage 'u8g2') -Filter '*.o' | Remove-Item -Force -ErrorAction SilentlyContinue }
exit $rc
