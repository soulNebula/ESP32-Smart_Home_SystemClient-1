[CmdletBinding()]
param(
    # 要干几件事
    [string[]]$Task = @('build'),

    [string]$Port = '',

    # 本工程的沙箱目录
    [string]$SandboxDir = '',

    # Python 环境位置
    [string]$PyEnv = '',

    # 编译用的英文镜像目录
    [string]$Mirror = '',

    # PlatformIO 包目录
    [string]$PioPackages = 'C:\Users\Administrator\.platformio\packages',

    # 强制重建 Python 环境
    [switch]$Reinstall
)

# 一个任务名要拆两半
$Task = @($Task | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
if ($Task.Count -eq 0) { $Task = @('build') }

$ErrorActionPreference = 'Continue'

$projectDir = (Get-Item (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))).FullName
if (-not $SandboxDir) { $SandboxDir = Join-Path $projectDir '.idf-sandbox' }
if (-not $PyEnv)      { $PyEnv      = Join-Path $SandboxDir 'penv' }
if (-not $Mirror)     { $Mirror     = Join-Path $env:TEMP 'esp32_smart_home' }
$logDir     = Join-Path $projectDir 'logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$logFile    = Join-Path $logDir 'idf_pio.log'

# 日志只留两份
if (Test-Path $logFile) {
    Move-Item -Path $logFile -Destination (Join-Path $logDir 'idf_pio.prev.log') -Force -ErrorAction SilentlyContinue
}

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color; Add-Content -Path $logFile -Value $msg -Encoding UTF8 }

Say ("=" * 78) 'DarkCyan'
Say "idf_pio.ps1  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  task=$($Task -join ',')" 'Cyan'
Say "项目       : $projectDir"
Say "本地沙箱   : $SandboxDir   （Python 环境 + IDF_TOOLS_PATH，只服务本工程）"
Say "构建镜像   : $Mirror"
Say ("=" * 78) 'DarkCyan'

# 找 IDF 和工具链
$idfPath = Join-Path $PioPackages 'framework-espidf'
$paths = @{
    idf      = $idfPath
    idfPy    = Join-Path $idfPath 'tools\idf.py'
    toolchain= Join-Path $PioPackages 'toolchain-xtensa-esp-elf\bin'
    cmake    = Join-Path $PioPackages 'tool-cmake\bin'
    ninja    = Join-Path $PioPackages 'tool-ninja'
    esptool  = Join-Path $PioPackages 'tool-esptoolpy'
}
$missing = @()
foreach ($k in $paths.Keys) { if (-not (Test-Path $paths[$k])) { $missing += "$k -> $($paths[$k])" } }
if ($missing.Count -gt 0) {
    Say '[FAIL] 缺少必要组件：' 'Red'
    $missing | ForEach-Object { Say "        $_" 'Red' }
    exit 1
}
Say "[OK]   IDF_PATH = $idfPath"
Say "       IDF 版本 = $(Get-Content (Join-Path $idfPath 'version.txt') -ErrorAction SilentlyContinue)"

# 把 Python 环境备好
$py  = Join-Path $PyEnv 'Scripts\python.exe'
$okStamp = Join-Path $PyEnv '.idf_deps_ok'

if ($Reinstall -and (Test-Path $PyEnv)) {
    Say '[..]   -Reinstall：删除旧 Python 环境' 'Yellow'
    Remove-Item -Recurse -Force $PyEnv -ErrorAction SilentlyContinue
}

if (-not (Test-Path $py)) {
    # 借 PlatformIO 的 Python
    $basePys = @(
        (Join-Path $PioPackages '..\penv\Scripts\python.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe')
    ) | Where-Object { Test-Path $_ }

    if ($basePys.Count -eq 0) { Say '[FAIL] 找不到可用的 python.exe 来创建虚拟环境' 'Red'; exit 1 }
    $basePy = (Get-Item $basePys[0]).FullName
    Say "[..]   创建 Python 环境：$PyEnv  (母体 $basePy)"
    & $basePy -m venv $PyEnv 2>&1 | ForEach-Object { Say "       $_" }
    if (-not (Test-Path $py)) { Say '[FAIL] venv 创建失败' 'Red'; exit 1 }
}

if (-not (Test-Path $okStamp)) {
    Say '[..]   安装 ESP-IDF Python 依赖（首次较慢，走清华镜像）'
    $req = Join-Path $idfPath 'tools\requirements\requirements.core.txt'
    & $py -m pip install --quiet --upgrade pip 2>&1 | ForEach-Object { Say "       $_" }
    # pypi.tuna.tsinghua.edu.cn/simple -r $req 2>&1 | ForEach-Object { Say "       $_" }
    & $py -m pip install --quiet -i https:
    if ($LASTEXITCODE -ne 0) {
        Say '[WARN] 清华镜像失败，改用默认 PyPI 重试' 'Yellow'
        & $py -m pip install --quiet -r $req 2>&1 | ForEach-Object { Say "       $_" }
    }
    if ($LASTEXITCODE -eq 0) {
        'ok' | Out-File $okStamp -Encoding ascii
        Say '[OK]   IDF Python 依赖安装完成'
    } else {
        Say '[FAIL] IDF Python 依赖安装失败（检查网络/pip）' 'Red'
        exit 1
    }
} else {
    Say '[OK]   Python 环境已就绪（跳过 pip 安装）'
}

$pyVer = & $py -c "import sys;print(sys.version.split()[0])" 2>&1
Say "[OK]   Python $pyVer"

# 铺好环境变量
$env:IDF_PATH              = $idfPath
# ★ 也放本工程沙箱里
$env:IDF_TOOLS_PATH        = Join-Path $SandboxDir 'idf_tools'
$env:IDF_PYTHON_ENV_PATH   = $PyEnv
# 不设版本号会崩
$env:ESP_IDF_VERSION       = '5.4'
# 关掉 ccache：它对非 ASCII 路径最敏感
$env:IDF_CCACHE_ENABLE     = '0'
# 跳过依赖版本核对
$env:IDF_PYTHON_CHECK_CONSTRAINTS = 'no'
# 关掉组件管理器
$env:IDF_COMPONENT_MANAGER = '0'
$env:PYTHONUTF8            = '1'
if (-not (Test-Path $env:IDF_TOOLS_PATH)) { New-Item -ItemType Directory -Path $env:IDF_TOOLS_PATH -Force | Out-Null }

# 拼路径要一项项来
$toolDirs = @(
    (Join-Path $PyEnv 'Scripts'),
    $paths.esptool,
    $paths.toolchain,
    $paths.cmake,
    $paths.ninja
)
$env:PATH = ($toolDirs -join ';') + ';' + $env:PATH

# 先自己查一遍工具
foreach ($t in @('cmake', 'ninja')) {
    $g = Get-Command $t -ErrorAction SilentlyContinue
    if ($g) {
        Say "[OK]   PATH: $t -> $($g.Source)"
    } else {
        Say "[FAIL] PATH 里找不到 $t" 'Red'
        Say "       当前 PATH = $env:PATH" 'Red'
        exit 1
    }
}

# 查编译器在不在
$gcc = Join-Path $paths.toolchain 'xtensa-esp32s3-elf-gcc.exe'
if (Test-Path $gcc) {
    $gccVer = (& $gcc --version 2>&1 | Select-Object -First 1)
    Say "[OK]   $gccVer"
} else {
    Say "[FAIL] 找不到 $gcc" 'Red'; exit 1
}

# 源码同步到镜像
Say "[..]   同步源码 -> $Mirror"
if (-not (Test-Path $Mirror)) { New-Item -ItemType Directory -Path $Mirror -Force | Out-Null }
# 沙箱不往镜像里拷
robocopy $projectDir $Mirror /MIR /XD build logs .git .tmp-crops .idf-sandbox android /XF sdkconfig.old /NFL /NDL /NJH /NJS /NP | Out-Null
$rc = $LASTEXITCODE
if ($rc -ge 8) { Say "[FAIL] robocopy 失败 (exit $rc)" 'Red'; exit 1 }
Say "[OK]   源码同步完成 (robocopy exit $rc)"
if (Test-Path (Join-Path $projectDir 'sdkconfig')) {
    Copy-Item (Join-Path $projectDir 'sdkconfig') (Join-Path $Mirror 'sdkconfig') -Force
    Say '[OK]   sdkconfig 已带入镜像'
}

# 改过的文件强制重编
$stampPath = "$Mirror.sync_stamp.json"
$stamp     = @{}
if (Test-Path -LiteralPath $stampPath) {
    try {
        $j = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
        foreach ($prop in $j.PSObject.Properties) { $stamp[$prop.Name] = [int64]$prop.Value }
    } catch { $stamp = @{} }
}
$isFirst = ($stamp.Count -eq 0)
$skipTop = @('build','logs','.git','.tmp-crops','.idf-sandbox','android')
$srcFiles = @()
foreach ($top in (Get-ChildItem -LiteralPath $projectDir -Force)) {
    if ($skipTop -contains $top.Name) { continue }
    if ($top.PSIsContainer) { $srcFiles += Get-ChildItem -LiteralPath $top.FullName -Recurse -File -Force }
    else                    { $srcFiles += $top }
}
$newStamp = @{}
$touched  = 0
foreach ($f in $srcFiles) {
    if ($f.Name -eq 'sdkconfig.old') { continue }
    $rel   = $f.FullName.Substring($projectDir.Length + 1)
    $ticks = $f.LastWriteTimeUtc.Ticks
    $newStamp[$rel] = $ticks
    if (-not $isFirst -and $stamp.ContainsKey($rel) -and $stamp[$rel] -eq $ticks) { continue }
    $dst = Join-Path $Mirror $rel
    if (Test-Path -LiteralPath $dst) {
        (Get-Item -LiteralPath $dst -Force).LastWriteTime = Get-Date
        $touched++
    }
}
try { $newStamp | ConvertTo-Json -Compress | Set-Content -LiteralPath $stampPath -Encoding UTF8 } catch { }
if ($isFirst) {
    Say "[OK]   首次建立同步清单：$touched 个源文件已强制重编（之后走增量）"
} elseif ($touched -gt 0) {
    Say "[OK]   $touched 个源文件有改动 -> 已 touch 镜像副本，强制重编"
}

# 最后跑 idf.py
$idfArgs = @((Join-Path $idfPath 'tools\idf.py'), '-C', $Mirror)
if ($Port) { $idfArgs += @('-p', $Port) }
$idfArgs += $Task

Say "[..]  执行: idf.py -C `"$Mirror`" $($Task -join ' ')" 'Cyan'
& $py @idfArgs 2>&1 | Tee-Object -FilePath $logFile -Append | ForEach-Object { Write-Host $_ }
$idfRc = $LASTEXITCODE

Say ("-" * 78) 'DarkCyan'
Say "idf.py 退出码 = $idfRc   日志: $logFile" $(if ($idfRc -eq 0) { 'Green' } else { 'Red' })

# 把配置改动带回来
$mirrorCfg = Join-Path $Mirror 'sdkconfig'
if ($idfRc -eq 0 -and (Test-Path $mirrorCfg)) {
    Copy-Item $mirrorCfg (Join-Path $projectDir 'sdkconfig') -Force
    Say '[OK]   sdkconfig 已回写到源码目录'
}
exit $idfRc
