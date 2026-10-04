# =============================================================================
#  android/build.ps1 -- build the SmartHome BLE Android app on Windows
# =============================================================================
#  WHY THIS SCRIPT EXISTS
#  ---------------------------------------------------------------------------
#  This project lives in a folder whose name contains Chinese characters:
#      C:\Users\Administrator\Desktop\esp32智能家居_客户\android
#  Android/Gradle *usually* cope with that, but AGP's aapt2 / lint / R8 have all
#  historically had trouble with non-ASCII paths and non-ASCII console code
#  pages. The ESP32 side of this repo solves the same problem by mirroring into
#  a pure-ASCII directory (see ../tools/run_idf.ps1), and we do the same here:
#
#      <source, Chinese path>  --robocopy-->  C:\smarthome_app  -->  gradle
#
#  The source of truth stays in the project directory. Only the build happens in
#  the mirror. The resulting APK is copied back into android\apk\.
#
#  If the project path is already pure ASCII the script builds in place.
#
#  USAGE (Windows PowerShell 5.1 -- pwsh is NOT installed on this machine)
#  ---------------------------------------------------------------------------
#    powershell -ExecutionPolicy Bypass -File android\build.ps1
#    powershell -ExecutionPolicy Bypass -File android\build.ps1 -Task assembleRelease
#    powershell -ExecutionPolicy Bypass -File android\build.ps1 -Offline
#    powershell -ExecutionPolicy Bypass -File android\build.ps1 -Mirror "D:\smarthome_app"
# =============================================================================
[CmdletBinding()]
param(
    # Example: -Task assembleDebug
    # NOTE: declared as a plain [string] on purpose. As a [string[]] parameter,
    # `-File script.ps1 -Task a,b` receives the single token "a,b" and gradle
    # then reports "Task 'a,b' not found". Run the script once per task instead.
    [string]$Task = 'assembleDebug',

    # Pure-ASCII build mirror used when the source path is not pure ASCII.
    [string]$Mirror = 'C:\smarthome_app',

    # Pass --offline to gradle (uses the local ~\.gradle cache only).
    [switch]$Offline,

    # Keep a persistent Gradle daemon alive. Default is OFF, because a daemon
    # inherits the stdout handle of the calling process and never releases it,
    # which makes any tool that captures this script's output (CI, an agent
    # harness, `Tee-Object`, ...) wait forever even though the build finished.
    # Use -Daemon only when running interactively in your own terminal.
    [switch]$Daemon,

    # Forward extra arguments to gradle verbatim.
    [string[]]$ExtraArgs = @(),

    # Force building in the source directory even if the path is non-ASCII.
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

# -----------------------------------------------------------------------------
# 1. locate the JDK
# -----------------------------------------------------------------------------
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

# -----------------------------------------------------------------------------
# 2. locate the Android SDK
# -----------------------------------------------------------------------------
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

# -----------------------------------------------------------------------------
# 3. locate gradle (wrapper jar first, then the cached distribution)
# -----------------------------------------------------------------------------
Write-Step 'Locating Gradle'
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$gradleCmd = $null

if ((Test-Path (Join-Path $src 'gradlew.bat')) -and (Test-Path (Join-Path $src 'gradle\wrapper\gradle-wrapper.jar'))) {
    $gradleCmd = Join-Path $src 'gradlew.bat'
    Write-Ok "Using wrapper: $gradleCmd"
} else {
    # Fall back to the Gradle distribution already unpacked in the user cache.
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

# -----------------------------------------------------------------------------
# 4. pick the build directory (mirror when the path is not pure ASCII)
# -----------------------------------------------------------------------------
$buildDir = $src
$useMirror = (-not $InPlace) -and (-not (Test-Ascii $src))

if ($useMirror) {
    Write-Step "Mirroring source to $Mirror"
    if (-not (Test-Ascii $Mirror)) { Write-Err "Mirror path must be pure ASCII: $Mirror"; exit 1 }
    New-Item -ItemType Directory -Force -Path $Mirror | Out-Null
    # /MIR keeps the mirror in sync; build outputs live in the mirror only.
    robocopy $src $Mirror /MIR /NFL /NDL /NJH /NJS /NP /XD apk build .gradle | Out-Null
    Write-Ok "Mirrored $src -> $Mirror"
    $buildDir = $Mirror
} else {
    Write-Warn 'Building in place (pure-ASCII path or -InPlace).'
}

# -----------------------------------------------------------------------------
# 5. build
# -----------------------------------------------------------------------------
Write-Step "Gradle $Task"
# -p (--project-dir) is passed explicitly so the build targets the mirror even
# if the wrapper script itself lives in the (possibly non-ASCII) source tree.
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
    # Interactive path: let the daemon own the console.
    & $gradleCmd @gradleArgs
    $code = $LASTEXITCODE
} else {
    # Redirect to a file rather than a pipe. A surviving child process holding
    # the write end of a pipe would keep this invocation blocked forever.
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

# -----------------------------------------------------------------------------
# 6. copy the APK back into <source>\apk
# -----------------------------------------------------------------------------
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
