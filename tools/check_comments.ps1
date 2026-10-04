# 模块：
#   编译前的注释检查工具。专门查"注释写法把代码结构搞坏"的毛病，
#   本项目因为这三种写法编译失败过三次：注释里出现结束符号、
#   注释里又嵌一个注释开头、注释某行以反斜杠收尾。
#   它只读文本，把每个 C、C++ 文件扫一遍并报出行号，编译前先跑它。
#   参数名不敢叫 Path：叫 Path 时相对路径会绑成空，换成别的名字就没事。
#   扫描时不在注释里却碰到结束符号，说明前面的注释提前关了，后面的话就成了
#   代码，报错还报在老远的地方。
#
# 功能：
#   扫描源文件
#   查三类注释毛病
#   查有没有注释没关
[CmdletBinding()]
param(
    [string]$Path = '',
    [string[]]$Ext = @('.c', '.h', '.cpp', '.hpp')
)

$ErrorActionPreference = 'Stop'

# 功能：参数名别叫 Path
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
$BS   = [char]92   # 功能：反斜杠
$SLASH = [char]47  # 功能：斜杠
$STAR  = [char]42  # 功能：星号
$DQ   = [char]34   # 功能：双引号
$SQ   = [char]39   # 功能：单引号

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
# 功能：查注释结束在哪儿
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
