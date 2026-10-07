[CmdletBinding()]
param(
    # 要干几件事
    # 也能一次给多个
    [string[]]$Task = @('build'),

    # 英文镜像目录
    [string]$Mirror = 'C:\esp32_smart_home',

    # 手动指定 IDF 路径
    [string]$IdfPath = '',

    # 手动指定工具目录
    [string]$ToolsPath = '',

    # 额外参数原样传
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Continue'

function Write-Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "  [OK]   $m" -ForegroundColor Green }
function Write-Warn($m) { Write-Host "  [WARN] $m" -ForegroundColor Yellow }
function Write-Err($m)  { Write-Host "  [FAIL] $m" -ForegroundColor Red }

function Test-Ascii([string]$s) {
    foreach ($ch in $s.ToCharArray()) { if ([int]$ch -gt 127) { return $false } }
    return $true
}

# 项目根就是脚本上一层
$projectDir = (Get-Item (Split-Path -Parent $PSScriptRoot)).FullName
Write-Step "Source project : $projectDir"

# 定下在哪儿编译
$buildRoot = $projectDir
$usingMirror = $false

if (-not (Test-Ascii $projectDir)) {
    Write-Warn 'Project path contains NON-ASCII characters.'
    Write-Warn 'ESP-IDF cannot build in place here (kconfig / ccache / ldgen all break).'
    Write-Warn "Mirroring sources to the ASCII path below and building THERE:"
    Write-Host  "         $Mirror" -ForegroundColor Yellow

    if (-not (Test-Ascii $Mirror)) {
        Write-Err "Mirror path is not ASCII either: $Mirror"
        exit 1
    }

    if (-not (Test-Path $Mirror)) {
        New-Item -ItemType Directory -Path $Mirror -Force | Out-Null
    }

    Write-Step '1) robocopy source -> mirror (excluding build/, logs)'
    # 连空目录一起拷
    # 跳过 build 保缓存
    # 日志文件不拷
    robocopy $projectDir $Mirror /E /XD build /XF build_log.txt build_log_ascii.txt build_log_subst.txt /NFL /NDL /NJH /NJS /NP | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -ge 8) {
        Write-Err "robocopy failed (exit $rc)"
        exit 1
    }
    Write-Ok "sources synced (robocopy exit $rc)"

    # 把配置带进镜像
    if (Test-Path (Join-Path $projectDir 'sdkconfig')) {
        Copy-Item (Join-Path $projectDir 'sdkconfig') (Join-Path $Mirror 'sdkconfig') -Force
        Write-Ok 'sdkconfig copied into the mirror'
    }

    $buildRoot  = $Mirror
    $usingMirror = $true
} else {
    Write-Ok 'Project path is pure ASCII - building in place.'
}

# 找一个 IDF 来用
if (-not $IdfPath) {
    $cand = @()
    if ($env:IDF_PATH) { $cand += $env:IDF_PATH }
    $cand += @(
        'E:\Espressif\frameworks\esp-idf-v5.4.4'
        'C:\Espressif\frameworks\esp-idf-v5.4.4'
        "$env:USERPROFILE\esp\esp-idf"
        'C:\esp\esp-idf'
    )
    foreach ($base in @('E:\Espressif\frameworks', 'C:\Espressif\frameworks')) {
        if (Test-Path $base) {
            Get-ChildItem $base -Directory -Filter 'esp-idf-*' -ErrorAction SilentlyContinue |
                ForEach-Object { $cand += $_.FullName }
        }
    }
    foreach ($c in $cand) {
        if ($c -and (Test-Path (Join-Path $c 'tools\idf.py'))) { $IdfPath = $c; break }
    }
}
if (-not $IdfPath -or -not (Test-Path (Join-Path $IdfPath 'tools\idf.py'))) {
    Write-Err 'ESP-IDF not found. Pass -IdfPath "E:\Espressif\frameworks\esp-idf-v5.4.4"'
    exit 1
}
Write-Ok "IDF_PATH = $IdfPath"

# 找工具目录并启用
if (-not $ToolsPath) {
    if ($env:IDF_TOOLS_PATH -and (Test-Path (Join-Path $env:IDF_TOOLS_PATH 'python_env'))) {
        $ToolsPath = $env:IDF_TOOLS_PATH
    } else {
        $guess = Split-Path -Parent (Split-Path -Parent $IdfPath)
        if (Test-Path (Join-Path $guess 'python_env')) { $ToolsPath = $guess }
    }
}
if ($ToolsPath) {
    $env:IDF_TOOLS_PATH = $ToolsPath
    Write-Ok "IDF_TOOLS_PATH = $ToolsPath"
}

$pyEnv = if ($ToolsPath) { Join-Path $ToolsPath 'python_env' } else { $null }
if ($pyEnv -and (Test-Path $pyEnv)) {
    $venv = Get-ChildItem $pyEnv -Directory -Filter 'idf*_py*_env' -ErrorAction SilentlyContinue |
            Select-Object -First 1
    if ($venv) {
        $scripts = Join-Path $venv.FullName 'Scripts'
        if (Test-Path $scripts) { $env:PATH = "$scripts;$env:PATH" }
    }
}

$env:PYTHONUTF8       = '1'
$env:PYTHONIOENCODING = 'utf-8'

if (Test-Path (Join-Path $IdfPath 'export.ps1')) {
    . (Join-Path $IdfPath 'export.ps1') 2>$null
}

if (-not (Get-Command riscv32-esp-elf-gcc -ErrorAction SilentlyContinue) -and $ToolsPath) {
    Write-Warn 'export.ps1 did not populate PATH; adding toolchain dirs manually'
    foreach ($pat in @((Join-Path $ToolsPath 'tools\riscv32-esp-elf'),
                       (Join-Path $ToolsPath 'tools\xtensa-esp-elf'))) {
        Get-ChildItem $pat -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            foreach ($sub in @('riscv32-esp-elf\bin', 'xtensa-esp-elf\bin')) {
                $b = Join-Path $_.FullName $sub
                if (Test-Path $b) { $env:PATH = "$b;$env:PATH" }
            }
        }
    }
    Get-ChildItem (Join-Path $ToolsPath 'tools\cmake') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $b = Join-Path $_.FullName 'bin'; if (Test-Path $b) { $env:PATH = "$b;$env:PATH" }
    }
    Get-ChildItem (Join-Path $ToolsPath 'tools\ninja') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $env:PATH = "$($_.FullName);$env:PATH"
    }
}

Write-Step 'Toolchain check'
foreach ($c in 'idf.py', 'cmake', 'ninja', 'riscv32-esp-elf-gcc') {
    $g = Get-Command $c -ErrorAction SilentlyContinue
    if ($g) { Write-Ok "$c -> $($g.Source)" } else { Write-Warn "$c not found" }
}

# 在编译目录跑命令
Set-Location $buildRoot

# 中文路径就关编译缓存
$idfArgs = @()
if (-not (Test-Ascii $buildRoot)) { $idfArgs += '--no-ccache' }
$idfArgs += $Task
if ($ExtraArgs.Count -gt 0) { $idfArgs += $ExtraArgs }

# 按逗号拆成两个参数
$idfArgs = @($idfArgs | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })

Write-Step "2) idf.py $($idfArgs -join ' ')   (in $buildRoot)"
& idf.py @idfArgs
$code = $LASTEXITCODE

# 收回配置改动
if ($usingMirror) {
    $mirrorCfg = Join-Path $Mirror 'sdkconfig'
    if (Test-Path $mirrorCfg) {
        Copy-Item $mirrorCfg (Join-Path $projectDir 'sdkconfig') -Force
        Write-Ok 'sdkconfig copied back to the source project'
    }
}

Write-Host ''
if ($code -eq 0) {
    Write-Host "=== idf.py $Task finished OK ===" -ForegroundColor Green
    if ($usingMirror -and $Task -in @('build', 'flash', 'monitor')) {
        Write-Host "    build artifacts: $Mirror\build" -ForegroundColor Green
    }
} else {
    Write-Host "=== idf.py $Task FAILED (exit $code) ===" -ForegroundColor Red
}
exit $code
