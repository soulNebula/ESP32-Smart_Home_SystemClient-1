# =============================================================================
#  build.ps1 -- build / flash / monitor helper for this project on Windows
# =============================================================================
#  THE PROBLEM
#  ---------------------------------------------------------------------------
#  This project sits in a folder whose name contains non-ASCII (Chinese)
#  characters. ESP-IDF v5.4 on Windows breaks in THREE separate places because
#  of that, and the build cannot succeed in place:
#
#   1. kconfig  -- tools/kconfig_new/prepare_kconfig_files.py opens
#      build/config.env via argparse.FileType('r'), which uses the *locale*
#      encoding (936/GBK here) while CMake writes it as UTF-8. The path is
#      decoded into mojibake and the build dies with:
#          FileNotFoundError: '.../build/kconfigs.in'
#      (the message even shows the correct path, because printing the mojibake
#       re-encodes it back into the original UTF-8 bytes)
#
#   2. ccache   -- compile commands are wrapped in `ccache <gcc>`, and ccache
#      converts the path with std::filesystem + the locale, then aborts with:
#          filesystem error: Cannot convert character sequence
#
#   3. ldgen    -- tools/ldgen/ldgen.py runs `xtensa-esp32s3-elf-objdump.exe`
#      with the .a path; the native tool mangles it and reports:
#          libxtensa.a: No such file or directory   (the file does exist)
#
#  NOTE: mapping an ASCII drive letter with `subst` does NOT help. CMake
#  resolves the drive back to the real path, so the Chinese path leaks through
#  anyway. This was tested.
#
#  THE FIX THIS SCRIPT IMPLEMENTS
#  ---------------------------------------------------------------------------
#  Mirror the source tree into a pure-ASCII directory and build THERE:
#      <source, Chinese path>  --robocopy-->  C:\esp32_smart_home  -->  idf.py
#
#  Your source of truth stays where it is; you keep editing it normally.
#  Only the build happens in the mirror. `sdkconfig` is copied in before the
#  build and copied back afterwards, so menuconfig settings are not lost.
#
#  If the project path is already pure ASCII, this script just builds in place
#  and the mirror is not used at all.
#
#  USAGE
#  ---------------------------------------------------------------------------
#    pwsh -File tools\build.ps1                  # build
#    pwsh -File tools\build.ps1 -Task flash,monitor
#    pwsh -File tools\build.ps1 -Task monitor
#    pwsh -File tools\build.ps1 -Task menuconfig
#    pwsh -File tools\build.ps1 -Task fullclean
#    pwsh -File tools\build.ps1 -Task erase-flash
#    pwsh -File tools\build.ps1 -Task build -Mirror "D:\esp_build"
# =============================================================================
[CmdletBinding()]
param(
    # build | flash | monitor | menuconfig | fullclean | set-target | erase-flash
    # 可以一次给多个，例如： -Task flash,monitor
    [string[]]$Task = @('build'),

    # Pure-ASCII directory used as the build mirror when the project path
    # contains non-ASCII characters.
    [string]$Mirror = 'C:\esp32_smart_home',

    # Explicit ESP-IDF checkout. Auto-detected when omitted.
    [string]$IdfPath = '',

    # Explicit IDF tools dir (contains python_env/ and tools/). Auto-detected.
    [string]$ToolsPath = '',

    # Extra args appended to idf.py verbatim.
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

# -----------------------------------------------------------------------------
# 0. project root = parent of this script's folder
# -----------------------------------------------------------------------------
$projectDir = (Get-Item (Split-Path -Parent $PSScriptRoot)).FullName
Write-Step "Source project : $projectDir"

# -----------------------------------------------------------------------------
# 1. decide where we actually build
# -----------------------------------------------------------------------------
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
    # /E        recurse incl. empty dirs
    # /XD build exclude any directory named "build" (keeps the mirror's build cache)
    # /XF       exclude log files
    robocopy $projectDir $Mirror /E /XD build /XF build_log.txt build_log_ascii.txt build_log_subst.txt /NFL /NDL /NJH /NJS /NP | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -ge 8) {
        Write-Err "robocopy failed (exit $rc)"
        exit 1
    }
    Write-Ok "sources synced (robocopy exit $rc)"

    # carry menuconfig settings in
    if (Test-Path (Join-Path $projectDir 'sdkconfig')) {
        Copy-Item (Join-Path $projectDir 'sdkconfig') (Join-Path $Mirror 'sdkconfig') -Force
        Write-Ok 'sdkconfig copied into the mirror'
    }

    $buildRoot  = $Mirror
    $usingMirror = $true
} else {
    Write-Ok 'Project path is pure ASCII - building in place.'
}

# -----------------------------------------------------------------------------
# 2. locate ESP-IDF
# -----------------------------------------------------------------------------
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

# -----------------------------------------------------------------------------
# 3. locate tools dir and activate
# -----------------------------------------------------------------------------
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

# -----------------------------------------------------------------------------
# 4. run idf.py in the build root
# -----------------------------------------------------------------------------
Set-Location $buildRoot

# ccache also chokes on non-ASCII paths, so disable it whenever we are not
# building in a pure-ASCII tree (i.e. only possible when not mirroring).
$idfArgs = @()
if (-not (Test-Ascii $buildRoot)) { $idfArgs += '--no-ccache' }
$idfArgs += $Task
if ($ExtraArgs.Count -gt 0) { $idfArgs += $ExtraArgs }

# ---------------------------------------------------------------------------
# ★ Expand "a,b" elements into separate arguments (2026-09 fix).
#   WHY: PowerShell's comma syntax `-ExtraArgs '-p','COM31'` builds a STRING
#   ARRAY whose elements are "-p" and "COM31" -- that part is fine. But when
#   such an array is splatted into a native executable (`& idf.py @idfArgs`),
#   Windows PowerShell 5.1 re-joins the elements with a COMMA instead of a
#   space, so idf.py literally receives the single token "-p,COM31" and dies:
#         Error: No such option: -p
#   That is exactly what the old documented flash command hit. Splitting every
#   element on commas here restores the intended behaviour, so BOTH of these
#   now work:
#         -ExtraArgs '-p','COM31'      # comma array
#         -ExtraArgs '-p','COM31'      # (same thing)
#         -Task erase-flash,flash      # already split by idf.py's own logic
#   Note this must run BEFORE the array is splatted, hence the copy below.
# ---------------------------------------------------------------------------
$idfArgs = @($idfArgs | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })

Write-Step "2) idf.py $($idfArgs -join ' ')   (in $buildRoot)"
& idf.py @idfArgs
$code = $LASTEXITCODE

# -----------------------------------------------------------------------------
# 5. copy sdkconfig back so menuconfig changes survive
# -----------------------------------------------------------------------------
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
