#!/usr/bin/env bash
# C/C++ 注释结构自检
#
# 专门抓「注释把代码结构搞坏」这一类问题：
#   1) 块注释里又出现了块注释起始符（嵌套）
#   2) 块注释里某行以反斜杠结尾（-Werror=comment）
#   3) 块注释被提前关闭，导致后面出现孤立的结束符 */
#   4) 块注释到文件结束还没闭合
#
# 这类问题的编译报错位置常常指到别处，很难查，所以烧录前值得跑一次
#
# 用法：
#   tools/verify/check_comments.sh                 检查整个工程
#   tools/verify/check_comments.sh <目录或文件>    只检查指定范围
#   tools/verify/check_comments.sh -x .h .hpp      只检查指定后缀（默认 .c .h .cpp .hpp）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
. "$SCRIPT_DIR/../_common.sh"

SCAN_ROOT=""
EXTS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -x|--ext) shift; while [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; do EXTS+=("$1"); shift; done ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "未知参数：$1" ;;
        *)  SCAN_ROOT="$1"; shift ;;
    esac
done

[ -n "$SCAN_ROOT" ] || SCAN_ROOT="$PROJECT_DIR"
[ -e "$SCAN_ROOT" ] || die "路径不存在：$SCAN_ROOT"
[ ${#EXTS[@]} -gt 0 ] || EXTS=('.c' '.h' '.cpp' '.hpp')

# 拼 find 的 -name 条件
NAME_ARGS=()
for e in "${EXTS[@]}"; do
    [ ${#NAME_ARGS[@]} -gt 0 ] && NAME_ARGS+=(-o)
    NAME_ARGS+=(-name "*$e")
done

step "C/C++ 注释结构自检"
info "范围 : $SCAN_ROOT"
info "后缀 : ${EXTS[*]}"

# 逐文件扫描，逐字符状态机，规则与 tools/verify/check_comments.ps1 一致
scan_file() {
    awk -v file="$1" '
    BEGIN {
        in_block = 0
        line = 1
        nprob = 0
        problems = ""
    }
    {
        # 每行独立判断一次「块注释里以反斜杠结尾」
        if (in_block && $0 ~ /\\[ \t]*$/) {
            problems = problems sprintf("  [PROBLEM] %s:%d  block comment line ends with backslash (-Werror=comment)\n", file, line)
            nprob++
        }

        n = length($0)
        i = 1
        while (i <= n) {
            c = substr($0, i, 1)
            d = (i < n) ? substr($0, i + 1, 1) : ""

            if (in_block) {
                if (c == "*" && d == "/") { in_block = 0; i += 2; continue }
                if (c == "/" && d == "*") {
                    problems = problems sprintf("  [PROBLEM] %s:%d  comment-start token INSIDE a block comment (nesting)\n", file, line)
                    nprob++
                    i += 2; continue
                }
                i++; continue
            }

            if (c == "/" && d == "*") { in_block = 1; i += 2; continue }
            if (c == "*" && d == "/") {
                problems = problems sprintf("  [PROBLEM] %s:%d  comment-END token OUTSIDE a comment -> a block comment was closed early\n", file, line)
                nprob++
                i += 2; continue
            }
            if (c == "/" && d == "/") break          # 行注释，剩下整行不管

            if (c == "\"" || c == "\x27") {          # 字符串 / 字符字面量
                q = c
                i++
                while (i <= n) {
                    ch = substr($0, i, 1)
                    if (ch == "\\") { i += 2; continue }
                    if (ch == q)    { i++; break }
                    i++
                }
                continue
            }
            i++
        }
        line++
    }
    END {
        if (in_block) {
            problems = problems sprintf("  [PROBLEM] %s:%d  block comment still OPEN at end of file\n", file, line)
            nprob++
        }
        printf "%s", problems
        exit (nprob > 0 ? 1 : 0)
    }' "$1"
}

checked=0
bad=0
while IFS= read -r f; do
    checked=$((checked + 1))
    if ! scan_file "$f"; then
        bad=$((bad + 1))
    fi
done < <(find "$SCAN_ROOT" -type f \( "${NAME_ARGS[@]}" \) \
            -not -path '*/build/*' \
            -not -path '*/managed_components/*' \
            -not -path '*/.idf-sandbox/*' \
            -not -path '*/.esp-sr-probe/*' \
            -not -path '*/u8g2/*' \
            -not -path '*/.git/*' \
            2>/dev/null | sort)

echo
if [ "$bad" -eq 0 ]; then
    ok "$checked 个文件检查完毕，注释结构没问题"
    exit 0
else
    err "$checked 个文件检查完毕，$bad 个文件有问题"
    exit 1
fi
