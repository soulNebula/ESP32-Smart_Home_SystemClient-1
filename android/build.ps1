[CmdletBinding()]
param(
    # 编哪个包，默认调试包
    [string]$Task = 'assembleDebug',

    # 纯粹的英文目录
    [string]$Mirror = 'C:\smarthome_app',

    # 只用本地缓存
    [switch]$Offline,

    # 默认不留常驻进程
    [switch]$Daemon,

    # 多出来的参数照传
    [string[]]$ExtraArgs = @(),

    # 就在原地编
    [switch]$InPlace
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

# 先找 JDK
Write-Step 'Locating JDK 21'
$jbr = 'C:\Program Files\Android\Android Studio\jbr'
if (Test-Path (Join-Path $jbr 'bin\java.exe')) {
    $env:JAVA_HOME = $jbr
    Write-Ok "JAVA_HOME = $jbr (Android Studio JBR)"
} elseif ($env:JAVA_HOME) {
    Write-Ok "JAVA_HOME = $env:JAVA_HOME"
} else {
    $java = Get-Command java -ErrorAction SilentlyContinue
    if (-not $java) { Write-Err 'No JDK found. Install JDK 17+ or Android Studio.'; exit 1 }
    Write-Warn "Using java from PATH: $($java.Source)"
}

# 再找安卓 SDK
Write-Step 'Locating Android SDK'
$sdkCandidates = @(
    $env:ANDROID_HOME,
    $env:ANDROID_SDK_ROOT,
    (Join-Path $env:LOCALAPPDATA 'Android\Sdk'),
    'C:\Android\Sdk'
) | Where-Object { $_ -and (Test-Path $_) }

if (-not $sdkCandidates) { Write-Err 'ANDROID_HOME not set and no SDK found.'; exit 1 }
$env:ANDROID_HOME     = $sdkCandidates[0]
$env:ANDROID_SDK_ROOT = $sdkCandidates[0]
Write-Ok "ANDROID_HOME = $($sdkCandidates[0])"

# 找编译工具
Write-Step 'Locating Gradle'
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$gradleCmd = $null

if ((Test-Path (Join-Path $src 'gradlew.bat')) -and (Test-Path (Join-Path $src 'gradle\wrapper\gradle-wrapper.jar'))) {
    $gradleCmd = Join-Path $src 'gradlew.bat'
    Write-Ok "Using wrapper: $gradleCmd"
} else {
    # 本地缓存里翻一个
    $dists = Join-Path $env:USERPROFILE '.gradle\wrapper\dists'
    $found = Get-ChildItem $dists -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\gradle.bat') } |
        Sort-Object Name -Descending
    if ($found) {
        $gradleCmd = Join-Path $found[0].FullName 'bin\gradle.bat'
        Write-Ok "Using cached distribution: $gradleCmd"
    } else {
        $onPath = Get-Command gradle -ErrorAction SilentlyContinue
        if ($onPath) { $gradleCmd = $onPath.Source; Write-Ok "Using gradle from PATH" }
    }
}
if (-not $gradleCmd) {
    Write-Err 'No gradle found. Run: gradle wrapper  (or install Gradle 8.9+).'
    exit 1
}

# 选在哪儿编
$buildDir = $src
$useMirror = (-not $InPlace) -and (-not (Test-Ascii $src))

if ($useMirror) {
    Write-Step "Mirroring source to $Mirror"
    if (-not (Test-Ascii $Mirror)) { Write-Err "Mirror path must be pure ASCII: $Mirror"; exit 1 }
    New-Item -ItemType Directory -Force -Path $Mirror | Out-Null
    # 整个抄过去
    robocopy $src $Mirror /MIR /NFL /NDL /NJH /NJS /NP /XD apk build .gradle | Out-Null
    Write-Ok "Mirrored $src -> $Mirror"
    $buildDir = $Mirror
} else {
    Write-Warn 'Building in place (pure-ASCII path or -InPlace).'
}

# 开始编译
Write-Step "Gradle $Task"
# 明确指到镜像目录
$gradleArgs = @('-p', $buildDir, $Task, '--console=plain', '-Dfile.encoding=UTF-8')
if (-not $Daemon)                        { $gradleArgs += '--no-daemon' }
if ($Offline)                            { $gradleArgs += '--offline' }
$gradleArgs += $ExtraArgs

$logPath = Join-Path $buildDir 'gradle-build.log'
Write-Host "  gradle $($gradleArgs -join ' ')" -ForegroundColor DarkGray
Write-Host "  log    $logPath" -ForegroundColor DarkGray

Push-Location $buildDir
$code = 0
if ($Daemon) {
    # 留常驻时直接跑
    & $gradleCmd @gradleArgs
    $code = $LASTEXITCODE
} else {
    # 日志写文件别用管道
    & $gradleCmd @gradleArgs *> $logPath
    $code = $LASTEXITCODE
    if (Test-Path $logPath) {
        Get-Content $logPath | ForEach-Object { Write-Host "  $_" }
    }
}
Pop-Location

if ($code -ne 0) {
    Write-Err "Gradle failed with exit code $code"
    Write-Host "  See $logPath" -ForegroundColor Yellow
    exit $code
}
Write-Ok "Gradle $Task succeeded"

# 把包收回来
Write-Step 'Collecting APKs'
$outDir = Join-Path $src 'apk'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$apkRoot = Join-Path $buildDir 'app\build\outputs\apk'
$apks = Get-ChildItem $apkRoot -Recurse -Filter '*.apk' -ErrorAction SilentlyContinue
if (-not $apks) {
    Write-Warn "No APK found under $apkRoot"
} else {
    foreach ($apk in $apks) {
        $variant = if ($apk.FullName -match '\\apk\\([^\\]+)\\') { $matches[1] } else { 'unknown' }
        $dest = Join-Path $outDir "SmartHomeBLE-$variant.apk"
        Copy-Item $apk.FullName $dest -Force
        $mb = [math]::Round((Get-Item $dest).Length / 1MB, 2)
        Write-Ok "$dest  ($mb MB)"
    }
}

Write-Host ''
Write-Ok 'Done. Install with:  adb install -r <apk path>'
