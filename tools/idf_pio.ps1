# =============================================================================
#  idf_pio.ps1 -- 用 PlatformIO 自带的 ESP-IDF 5.4.0 构建本项目
# =============================================================================
#  背景：本机原来的 ESP-IDF（E:\Espressif\...）已被删除。PlatformIO 的
#  packages 目录里有一套完整的 ESP-IDF 5.4.0 + xtensa-esp-elf 工具链，
#  本脚本把它"组装"成一个可用的 idf.py 环境，不下载任何东西（除 pip 依赖）：
#
#     IDF_PATH     = <platformio>/packages/framework-espidf        (5.4.0)
#     工具链/CMake/Ninja/esptool = <platformio>/packages/...
#     Python 虚拟环境 = %TEMP%\esp32_idf_penv   （pip 装 IDF 的 requirements）
#     构建镜像目录   = %TEMP%\esp32_smart_home  （纯 ASCII，中文路径不能直接编译）
#
#  用法：
#     pwsh -File tools\idf_pio.ps1 -Task build
#     pwsh -File tools\idf_pio.ps1 -Task flash -Port COM31
#     pwsh -File tools\idf_pio.ps1 -Task erase-flash,flash -Port COM31
#     pwsh -File tools\idf_pio.ps1 -Task fullclean
#     pwsh -File tools\idf_pio.ps1 -Task size
#
#  说明：本脚本只做"环境组装 + 调用 idf.py"，源码始终以中文目录为准，
#        每次构建前把源码同步到 ASCII 镜像目录。
# =============================================================================
[CmdletBinding()]
param(
    # build | flash | erase-flash | fullclean | size | reconfigure | set-target ...
    [string[]]$Task = @('build'),

    [string]$Port = '',

    # ★ 本工程专属沙箱目录（默认 = <项目>\.idf-sandbox）。
    #   里面放 Python 环境 + IDF_TOOLS_PATH，全部只服务本工程：
    #   不装全局 IDF、不写系统/用户环境变量，删掉这个目录就算彻底卸载。
    [string]$SandboxDir = '',

    # Python 虚拟环境（默认 <沙箱>\penv）
    [string]$PyEnv = '',

    # 构建镜像目录。★ 必须纯 ASCII：
    #   本工程源码路径含中文，IDF 的 kconfig/ccache/ldgen 会连环报错，
    #   所以只能把源码镜像到 ASCII 目录里编译（这是项目 README 早就记录的坑）。
    #   %TEMP% 是 ASCII 且系统自带；想换位置用 -Mirror "D:\esp_build"。
    [string]$Mirror = '',

    # PlatformIO packages 根目录
    [string]$PioPackages = 'C:\Users\Administrator\.platformio\packages',

    # 强制重建 Python 环境
    [switch]$Reinstall
)

# ★ 用 `powershell -File tools\run_idf.ps1 -Task build,flash` 调用时，PowerShell 会把
#   "build,flash" 当成【一个字符串】传给 [string[]]，而 idf.py 需要的是两个独立参数
#   （否则 ninja 会报 unknown target 'build,flash'）。这里统一再拆一次逗号。
$Task = @($Task | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
if ($Task.Count -eq 0) { $Task = @('build') }

$ErrorActionPreference = 'Continue'

$projectDir = (Get-Item (Split-Path -Parent $PSScriptRoot)).FullName
if (-not $SandboxDir) { $SandboxDir = Join-Path $projectDir '.idf-sandbox' }
if (-not $PyEnv)      { $PyEnv      = Join-Path $SandboxDir 'penv' }
if (-not $Mirror)     { $Mirror     = Join-Path $env:TEMP 'esp32_smart_home' }
$logDir     = Join-Path $projectDir 'logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$logFile    = Join-Path $logDir 'idf_pio.log'

# ★ 日志轮转：以前是纯追加，构建几十次就涨到 1MB（实测 946KB）。
#   现在每轮把上一份挪成 idf_pio.prev.log，只保留"本次构建 + 上一次"两份。
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

# ---------------------------------------------------------------------------
# 1. 定位 PlatformIO 里的 ESP-IDF / 工具链
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# 2. Python 环境（pip 装 IDF requirements）
# ---------------------------------------------------------------------------
$py  = Join-Path $PyEnv 'Scripts\python.exe'
$okStamp = Join-Path $PyEnv '.idf_deps_ok'

if ($Reinstall -and (Test-Path $PyEnv)) {
    Say '[..]   -Reinstall：删除旧 Python 环境' 'Yellow'
    Remove-Item -Recurse -Force $PyEnv -ErrorAction SilentlyContinue
}

if (-not (Test-Path $py)) {
    # 用 PlatformIO 自带的 python 作为"母体"创建 venv（不需要系统 python）
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
    & $py -m pip install --quiet -i https://pypi.tuna.tsinghua.edu.cn/simple -r $req 2>&1 | ForEach-Object { Say "       $_" }
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

# ---------------------------------------------------------------------------
# 3. 环境变量（模拟 IDF 的 export.ps1）
# ---------------------------------------------------------------------------
$env:IDF_PATH              = $idfPath
$env:IDF_TOOLS_PATH        = Join-Path $SandboxDir 'idf_tools'   # ★ 也放本工程沙箱里
$env:IDF_PYTHON_ENV_PATH   = $PyEnv
# idf_component_manager 要读 ESP_IDF_VERSION（正常由 IDF 的 export 脚本写入）。
# 不设它会直接崩：Version.coerce(None) → TypeError: expected string, got NoneType
$env:ESP_IDF_VERSION       = '5.4'
$env:IDF_CCACHE_ENABLE     = '0'          # 关掉 ccache：它对非 ASCII 路径最敏感
# 没跑过官方 install 脚本 → 没有 espidf.constraints.v5.4.txt。
# IDF 自己的 Dockerfile 就是这么关掉约束检查的（依赖已由 pip 装好）。
$env:IDF_PYTHON_CHECK_CONSTRAINTS = 'no'
# ★ 关掉组件管理器：IDF 5.4.0 的 cmake 用 --interface_version=3 调
#   idf_component_manager，而 pip 随手装的最新版只认 4/5/6 → 配置阶段直接 FATAL。
#   本工程没有任何远程组件依赖（dependencies.lock 里只有 idf 本身，
#   唯一的 idf_component.yml 只声明 IDF 版本），所以关掉它完全无损。
#   （IDF 官方支持这个开关，见 tools/cmake/build.cmake:623）
$env:IDF_COMPONENT_MANAGER = '0'
$env:PYTHONUTF8            = '1'
if (-not (Test-Path $env:IDF_TOOLS_PATH)) { New-Item -ItemType Directory -Path $env:IDF_TOOLS_PATH -Force | Out-Null }

# ★ 注意：这里必须用 Join-Path / 括号逐项写，不能写成 @($PyEnv + '\Scripts', $a, $b)。
#   原因：PowerShell 里 `,`（数组构造）的优先级【高于】`+`，上面那种写法会被解析成
#   $PyEnv + ('\Scripts', $a, $b)，而"字符串 + 数组"是按 $OFS（默认空格）拼接的
#   → 整条 PATH 被空格连成一段，cmake/ninja 全都找不到（本机实测踩过）。
$toolDirs = @(
    (Join-Path $PyEnv 'Scripts'),
    $paths.esptool,
    $paths.toolchain,
    $paths.cmake,
    $paths.ninja
)
$env:PATH = ($toolDirs -join ';') + ';' + $env:PATH

# 构建工具自检：idf.py 是用 PATH 去找 cmake / ninja 的，找不到就只报一句
# "cmake must be available on the PATH"（看不出到底缺哪个），所以这里自己先查一遍
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

# 工具链自检
$gcc = Join-Path $paths.toolchain 'xtensa-esp32s3-elf-gcc.exe'
if (Test-Path $gcc) {
    $gccVer = (& $gcc --version 2>&1 | Select-Object -First 1)
    Say "[OK]   $gccVer"
} else {
    Say "[FAIL] 找不到 $gcc" 'Red'; exit 1
}

# ---------------------------------------------------------------------------
# 4. 同步源码到 ASCII 镜像目录
# ---------------------------------------------------------------------------
Say "[..]   同步源码 -> $Mirror"
if (-not (Test-Path $Mirror)) { New-Item -ItemType Directory -Path $Mirror -Force | Out-Null }
# ★ /XD 必须排除 .idf-sandbox：里面是 Python 环境（上千个文件、上百 MB），
#   同步进镜像既慢又没意义。
# ★ 用 /MIR（= /E + /PURGE）而不是 /E：源码里删掉的文件/组件必须在镜像里也消失，
#   否则镜像会一直留着旧副本（实测删掉 components/Middlewares 后，IDF 仍在报
#   ".../components/Middlewares does not contain a CMakeLists.txt"）。build/ 等
#   已用 /XD 排除，不会被 purge，增量编译不受影响。
# ★ android/ 也排除：IDF 构建完全用不到它（里面是几 MB 的 APK 和 gradle 缓存），
#   同步进去只是白白拖慢每次构建。
robocopy $projectDir $Mirror /MIR /XD build logs .git .tmp-crops .idf-sandbox android /XF sdkconfig.old /NFL /NDL /NJH /NJS /NP | Out-Null
$rc = $LASTEXITCODE
if ($rc -ge 8) { Say "[FAIL] robocopy 失败 (exit $rc)" 'Red'; exit 1 }
Say "[OK]   源码同步完成 (robocopy exit $rc)"
if (Test-Path (Join-Path $projectDir 'sdkconfig')) {
    Copy-Item (Join-Path $projectDir 'sdkconfig') (Join-Path $Mirror 'sdkconfig') -Force
    Say '[OK]   sdkconfig 已带入镜像'
}

# ---------------------------------------------------------------------------
# 4b. 防"镜像陈旧对象"：把本次内容变过的源码在镜像里的时间戳刷成"现在"
# ---------------------------------------------------------------------------
# 【为什么需要】robocopy 会保留源文件的修改时间。如果源码改动发生在"某次编译完成之前、
#   本次同步之后"的窗口里（最典型：编译还在跑的时候改了源码），拷进镜像的源码 mtime
#   会比已经编好的 .obj 更旧，ninja 就判定"不用重编" —— 结果烧进板子的、以及日志里
#   看到的，全是旧固件，而 build 还是 exit 0，完全看不出来。
#   2026-09-29 实测踩到：ADKEY 告警那条改动没进固件，白烧了一轮才发现。
# 【做法】在镜像【外面】存一份 <相对路径, 源文件 mtime ticks> 清单（放镜像里会被
#   /MIR 的 /PURGE 删掉）。清单对不上的文件 = 本次内容有变 → 把镜像副本 touch 成
#   "现在"，强制 ninja 重编；其余文件不动，增量编译照旧。
#   清单不存在（首次启用）时保守处理：全部 touch，多花一次全量编译，换"镜像一定不陈旧"。
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

# ---------------------------------------------------------------------------
# 5. 跑 idf.py
# ---------------------------------------------------------------------------
$idfArgs = @((Join-Path $idfPath 'tools\idf.py'), '-C', $Mirror)
if ($Port) { $idfArgs += @('-p', $Port) }
$idfArgs += $Task

Say "[..]  执行: idf.py -C `"$Mirror`" $($Task -join ' ')" 'Cyan'
& $py @idfArgs 2>&1 | Tee-Object -FilePath $logFile -Append | ForEach-Object { Write-Host $_ }
$idfRc = $LASTEXITCODE

Say ("-" * 78) 'DarkCyan'
Say "idf.py 退出码 = $idfRc   日志: $logFile" $(if ($idfRc -eq 0) { 'Green' } else { 'Red' })

# 构建成功时回写 sdkconfig（menuconfig 改动不丢）
$mirrorCfg = Join-Path $Mirror 'sdkconfig'
if ($idfRc -eq 0 -and (Test-Path $mirrorCfg)) {
    Copy-Item $mirrorCfg (Join-Path $projectDir 'sdkconfig') -Force
    Say '[OK]   sdkconfig 已回写到源码目录'
}
exit $idfRc
