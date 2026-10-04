# =============================================================================
#  check_comments.ps1 -- static check for C/C++ comment structure (no compile)
# =============================================================================
#  WHY THIS SCRIPT EXISTS
#  ---------------------------------------------------------------------------
#  While integrating ESP-SR this project hit the SAME class of build failure
#  three times in a row, and none of them was a logic bug -- the *comment text
#  itself* was breaking the code structure, and gcc pointed at the wrong line:
#
#    1. a block comment line ending with a backslash  -> -Werror=comment
#    2. a block comment containing the 2-char comment-start token
#                                                     -> -Werror=comment
#    3. a block comment containing  "wn*" immediately followed by "/mn*"
#       -> the star+slash inside was read as the comment END, so the Chinese
#          text after it became real code:
#              error: unknown type name 'mn'
#              error: stray '\343' in program
#
#  So run this before building. It is a plain text scanner: it tracks whether
#  it is inside a block comment / line comment / string literal and reports:
#    - a comment-start token found INSIDE a block comment (nesting)
#    - a line ending in backslash INSIDE a block comment
#    - a block comment still open at end of file
#
#  NOTE: this file is intentionally pure ASCII. Windows PowerShell 5.1 decodes
#  a BOM-less .ps1 using the ANSI code page (GBK here), so a UTF-8 Chinese
#  script would be mojibake and fail to parse. Keep it ASCII.
#
#  USAGE
#  ---------------------------------------------------------------------------
#     powershell -ExecutionPolicy Bypass -File tools\check_comments.ps1
#     powershell -ExecutionPolicy Bypass -File tools\check_comments.ps1 -Path components
#
#  Exit code: 0 = clean, 1 = problems found (usable as a pre-build gate).
# =============================================================================
[CmdletBinding()]
param(
    [string]$Path = '',
    [string[]]$Ext = @('.c', '.h', '.cpp', '.hpp')
)

$ErrorActionPreference = 'Stop'

# NOTE: the param is deliberately NOT called $Path -- PowerShell's $PWD/$Path
# interaction plus relative-argument binding made `-Path components\BSP`
# resolve to null here (observed). $ScanRoot is unambiguous.
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
$BS   = [char]92   # backslash
$SLASH = [char]47  # /
$STAR  = [char]42  # *
$DQ   = [char]34   # "
$SQ   = [char]39   # '

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
        # ------------------------------------------------------------------
        #  CRITICAL: a comment-END token while NOT inside a comment means some
        #  earlier block comment was closed too early. This is exactly how the
        #  "wn*/mn*" bug manifested: the star+slash inside the text closed the
        #  comment, the rest of the Chinese line became code, and gcc reported
        #  it far away as "unknown type name" / "stray '\343'".
        #  Without this branch the scanner silently passed that case, because
        #  a valid early close leaves no unbalanced state behind.
        # ------------------------------------------------------------------
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
