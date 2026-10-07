[CmdletBinding()]
param(
    [string]$Path = '',
    [string[]]$Ext = @('.c', '.h', '.cpp', '.hpp')
)

$ErrorActionPreference = 'Stop'

# 参数名别叫 Path
$ScanRoot = $Path
if (-not $ScanRoot) { $ScanRoot = (Get-Item (Split-Path -Parent $PSScriptRoot)).FullName }
$resolved = Resolve-Path -LiteralPath $ScanRoot -ErrorAction SilentlyContinue
if (-not $resolved) { $resolved = Resolve-Path -LiteralPath (Join-Path (Get-Location).Path $ScanRoot) -ErrorAction SilentlyContinue }
if (-not $resolved) {
    Write-Host "[FAIL] cannot resolve scan root: $ScanRoot" -ForegroundColor Red
    exit 1
}
$root = $resolved.Path

$LF   = [char]10
$CR   = [char]13
$TAB  = [char]9
$NUL  = [char]0
# 反斜杠
$BS   = [char]92
# 斜杠
$SLASH = [char]47
# 星号
$STAR  = [char]42
# 双引号
$DQ   = [char]34
# 单引号
$SQ   = [char]39

Write-Host ""
Write-Host "=== C/C++ comment structure check ===" -ForegroundColor Cyan
Write-Host "  root : $root"
Write-Host "  types: $($Ext -join ' ')"
Write-Host ""

$files = Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $Ext -contains $_.Extension } |
    Where-Object { $_.FullName -notmatch '\\build\\|\\managed_components\\|\\\.idf-sandbox\\|\\u8g2\\|\\\.esp-sr-probe\\' }

$problemCount = 0
$checked = 0

foreach ($f in $files) {
    $checked++
    $text = [System.IO.File]::ReadAllText($f.FullName, [System.Text.Encoding]::UTF8)
    $len = $text.Length
    $line = 1
    $i = 0
    $inBlock = $false
    $issues = New-Object System.Collections.Generic.List[string]

    while ($i -lt $len) {
        $c = $text[$i]
        $n = if ($i + 1 -lt $len) { $text[$i + 1] } else { $NUL }

        if ($c -eq $LF) { $line++; $i++; continue }

        if ($inBlock) {
            if ($c -eq $STAR -and $n -eq $SLASH) { $inBlock = $false; $i += 2; continue }
            if ($c -eq $SLASH -and $n -eq $STAR) {
                $issues.Add(("{0}:{1}  comment-start token INSIDE a block comment (nesting)" -f $f.FullName, $line))
                $i += 2; continue
            }
            if ($c -eq $BS) {
                $j = $i + 1
                while ($j -lt $len -and ($text[$j] -eq ' ' -or $text[$j] -eq $TAB -or $text[$j] -eq $CR)) { $j++ }
                if ($j -lt $len -and $text[$j] -eq $LF) {
                    $issues.Add(("{0}:{1}  line ends with backslash INSIDE a block comment (-Werror=comment)" -f $f.FullName, $line))
                }
            }
            $i++; continue
        }

        if ($c -eq $SLASH -and $n -eq $STAR) { $inBlock = $true; $i += 2; continue }
# 查注释结束在哪儿
        if ($c -eq $STAR -and $n -eq $SLASH) {
            $issues.Add(("{0}:{1}  comment-END token OUTSIDE a comment -> a block comment was closed early" -f $f.FullName, $line))
            $i += 2; continue
        }
        if ($c -eq $SLASH -and $n -eq $SLASH) {
            while ($i -lt $len -and $text[$i] -ne $LF) { $i++ }
            continue
        }
        if ($c -eq $DQ -or $c -eq $SQ) {
            $q = $c; $i++
            while ($i -lt $len -and $text[$i] -ne $q) {
                if ($text[$i] -eq $BS) { $i++ }
                if ($i -lt $len -and $text[$i] -eq $LF) { $line++ }
                $i++
            }
            $i++; continue
        }
        $i++
    }

    if ($inBlock) {
        $issues.Add(("{0}:{1}  block comment still OPEN at end of file" -f $f.FullName, $line))
    }
    if ($issues.Count -gt 0) {
        $problemCount += $issues.Count
        foreach ($s in $issues) { Write-Host "  [PROBLEM] $s" -ForegroundColor Red }
    }
}

Write-Host ""
if ($problemCount -eq 0) {
    Write-Host "  [OK]   $checked file(s) checked, comment structure is clean" -ForegroundColor Green
    Write-Host ""
    exit 0
} else {
    Write-Host "  [FAIL] $checked file(s) checked, $problemCount problem(s) found" -ForegroundColor Red
    Write-Host ""
    exit 1
}
