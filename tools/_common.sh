#!/usr/bin/env bash
# 公共函数库：定位工程、准备 ESP-IDF 环境、彩色输出
# 由 tools/ 下的其它脚本 source，不单独运行
#
# 可用环境变量覆盖：
#   IDF_PATH         指定 ESP-IDF 路径（export.sh 所在目录）
#   IDF_TOOLS_PATH   指定工具链目录（默认 ~/.espressif）
#   COM_PORT         默认串口（默认自动探测）

# 已经 source 过就别重复
if [ -n "${__SH_COMMON_LOADED:-}" ]; then
    return 0
fi
__SH_COMMON_LOADED=1

# 本文件所在目录 = tools/
_COMMON_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 工程根目录 = tools/ 的上一层
PROJECT_DIR="$(cd "$_COMMON_TOOLS_DIR/.." && pwd)"
export PROJECT_DIR
# 脚本多数在 tools/<分类>/ 下，统一给出本库的路径，source 它用这个变量
COMMON_SH="$_COMMON_TOOLS_DIR/_common.sh"
export COMMON_SH
# 走 PlatformIO 回退方案时，这里放"能跑 idf 的 python"，用来执行 python -m idf
IDF_PYTHON=""
export IDF_PYTHON

# ---------- 输出 ----------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    _C_CYAN=$'\033[36m'; _C_GREEN=$'\033[32m'; _C_YELLOW=$'\033[33m'
    _C_RED=$'\033[31m';  _C_DIM=$'\033[2m';   _C_OFF=$'\033[0m'
else
    _C_CYAN=''; _C_GREEN=''; _C_YELLOW=''; _C_RED=''; _C_DIM=''; _C_OFF=''
fi

step() { printf '\n%s=== %s ===%s\n' "$_C_CYAN" "$*" "$_C_OFF"; }
ok()   { printf '%s  [OK]  %s%s\n' "$_C_GREEN" "$*" "$_C_OFF"; }
warn() { printf '%s  [WARN] %s%s\n' "$_C_YELLOW" "$*" "$_C_OFF"; }
err()  { printf '%s  [FAIL] %s%s\n' "$_C_RED" "$*" "$_C_OFF" >&2; }
info() { printf '%s  %s%s\n' "$_C_DIM" "$*" "$_C_OFF"; }

die() { err "$*"; exit 1; }

# ---------- 定位 ESP-IDF ----------
# 找到 export.sh 就 source 进来；已有 idf.py 就直接用
# 找不到标准安装时，会退回复用 PlatformIO 自带的那套 ESP-IDF（含工具链）
prepare_idf() {
    if command -v idf.py >/dev/null 2>&1; then
        ok "idf.py 已在 PATH：$(command -v idf.py)"
        return 0
    fi

    local candidates=()
    [ -n "${IDF_PATH:-}" ] && candidates+=("$IDF_PATH")
    candidates+=(
        "$HOME/esp/esp-idf"
        "$HOME/esp-idf"
        "/opt/esp-idf"
        "/opt/esp/esp-idf"
        "/usr/local/esp-idf"
    )

    local idf=""
    local c
    for c in "${candidates[@]}"; do
        if [ -f "$c/export.sh" ]; then
            idf="$c"
            break
        fi
    done

    if [ -n "$idf" ]; then
        step "加载 ESP-IDF 环境"
        info "IDF_PATH = $idf"
        # export.sh 会往终端输出一堆东西，压掉噪音
        # shellcheck disable=SC1090
        if ! . "$idf/export.sh" >/dev/null 2>&1; then
            # 失败时重跑一次，让报错露出来
            # shellcheck disable=SC1090
            . "$idf/export.sh" || return 1
        fi
        command -v idf.py >/dev/null 2>&1 || die "export.sh 跑完了但 PATH 里还是没有 idf.py"
        ok "idf.py -> $(command -v idf.py)"
        return 0
    fi

    # ---- 回退方案：复用 PlatformIO 自带的 ESP-IDF ----
    # 工程原来的 Windows 脚本就是这么干的，Linux 下同样适用
    if prepare_idf_from_pio; then
        return 0
    fi

    err "找不到 ESP-IDF"
    info "已尝试这些位置："
    for c in "${candidates[@]}"; do info "  $c"; done
    info "PlatformIO 目录：${PIO_PACKAGES:-$HOME/.platformio/packages}（没有 framework-espidf）"
    info ""
    info "两种解决办法，任选其一："
    info "  1) 装 ESP-IDF（推荐，最省事）："
    info "       mkdir -p ~/esp && cd ~/esp"
    info "       git clone -b v5.4.4 --recursive https://github.com/espressif/esp-idf.git"
    info "       cd esp-idf && ./install.sh esp32s3"
    info "     然后： export IDF_PATH=\"\$HOME/esp/esp-idf\""
    info "  2) 装 PlatformIO（会自动带上 ESP-IDF + 工具链），或用 PIO_PACKAGES 指定已有路径"
    info ""
    info "详见： $PROJECT_DIR/tools/build/setup.sh"
    return 1
}

# 用 PlatformIO 包目录里的 ESP-IDF + 工具链凑出可编译环境
prepare_idf_from_pio() {
    local pio="${PIO_PACKAGES:-$HOME/.platformio/packages}"
    local idf="$pio/framework-espidf"

    if [ ! -f "$idf/tools/idf.py" ]; then
        return 1
    fi

    step "未找到标准 ESP-IDF，改用 PlatformIO 自带的"
    info "PIO 包目录 : $pio"
    info "IDF        : $idf"
    local ver=""
    [ -f "$idf/version.txt" ] && ver="$(cat "$idf/version.txt")"
    info "IDF 版本   : ${ver:-?}"

    export IDF_PATH="$idf"
    export ESP_IDF_VERSION="${ESP_IDF_VERSION:-5.4}"
    # 关掉 ccache 与组件管理器版本核对（与 Windows 脚本保持一致，少踩坑）
    export IDF_CCACHE_ENABLE="${IDF_CCACHE_ENABLE:-0}"
    export IDF_PYTHON_CHECK_CONSTRAINTS="${IDF_PYTHON_CHECK_CONSTRAINTS:-no}"

    # 找 python：优先 PlatformIO 自带的那套，其次系统 python3
    # 注意要排除 Windows Store 的 python3.exe 转发器（0 字节，跑不了）
    local py=""
    local cand
    for cand in "$pio/../penv/bin/python" "$pio/penv/bin/python" \
                "$pio/../penv/Scripts/python.exe" \
                "$(command -v python3 2>/dev/null)" \
                "$(command -v python 2>/dev/null)"; do
        [ -n "$cand" ] || continue
        if [ -x "$cand" ] && "$cand" -c 'import sys' >/dev/null 2>&1; then
            py="$cand"
            break
        fi
    done
    [ -n "$py" ] || { err "找不到可用的 python（PlatformIO 自带的和系统的都试过了）"; return 1; }
    info "Python     : $py"

    # ★ 光有 IDF 源码和工具链还不够：idf.py 需要一批 python 依赖
    #   （PyYAML / click / esp-idf-kconfig …）PlatformIO 自带的 python 通常没有这些，
    #   所以这里必须先验证，否则会在后面报一堆看不懂的错
    if ! "$py" -c 'import yaml' >/dev/null 2>&1; then
        warn "这个 python 缺少 IDF 的依赖（PyYAML 都没有），idf.py 跑不起来"
        info "解决办法（任选其一）："
        info "  1) 推荐：装一份完整 ESP-IDF，它会自带整套 python 环境："
        info "       cd ~/esp/esp-idf && ./install.sh esp32s3"
        info "     之后跑 tools/build/build.sh 就会自动用它"
        info "  2) 给这个 python 补依赖（路径按你机器上的来）："
        info "       \"$py\" -m pip install -r \"$idf/tools/requirements/requirements.core.txt\""
        return 1
    fi

    # 工具链目录全部塞进 PATH
    # 注意各自的层级不一样：toolchain-* 和 tool-cmake 要进到 bin/，
    # tool-ninja 的可执行文件直接在包根目录
    local tool
    for tool in toolchain-xtensa-esp-elf/bin toolchain-xtensa-esp32s3-elf/bin \
                tool-cmake/bin tool-ninja tool-esptoolpy \
                tool-xtensa-esp-elf-gdb/bin tool-riscv32-esp-elf-gdb/bin; do
        [ -d "$pio/$tool" ] && PATH="$pio/$tool:$PATH"
    done
    export PATH

    # 检查关键工具
    local missing=()
    for tool in cmake ninja; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    if ! command -v xtensa-esp32s3-elf-gcc >/dev/null 2>&1; then
        missing+=("xtensa-esp32s3-elf-gcc")
    fi
    if [ ${#missing[@]} -gt 0 ]; then
        err "PlatformIO 包里缺少：${missing[*]}"
        info "装一次 PlatformIO 的工具链即可，或改用标准 ESP-IDF"
        return 1
    fi

    ok "工具链就绪（cmake / ninja / xtensa-esp32s3-elf-gcc 都找到了）"
    # PlatformIO 的 idf.py 头一行是 #!/usr/bin/env python，
    # Linux 上通常只有 python3，所以不用它自带的 shebang，统一走 python -m idf
    export IDF_PYTHON="$py"
    return 0
}


# 把要执行的 idf 命令写进调用方给的数组名里
# 正常情况下就是 idf.py；走了 PlatformIO 回退方案时用
#   "<python>" "<IDF_PATH>/tools/idf.py"
# 这样不依赖 idf.py 的 shebang（它的头一行是 #!/usr/bin/env python，
# Linux 上常常只有 python3，会找不到解释器）
# 用法： idf_cmd_arr CMD      # 之后用 "${CMD[@]}"
idf_cmd_arr() {
    local -n _out="$1"
    if [ -n "${IDF_PYTHON:-}" ]; then
        _out=("$IDF_PYTHON" "$IDF_PATH/tools/idf.py")
    else
        _out=(idf.py)
    fi
}

# ---------- 自动探测串口 ----------
# 按常见 ESP32 USB 转串口芯片挑，挑不到就报错让用户显式指定
detect_port() {
    local candidates=()
    local p

    # Linux 常见命名
    if [ -d /dev/serial/by-id ]; then
        while IFS= read -r p; do
            candidates+=("$p")
        done < <(ls -1 /dev/serial/by-id/ 2>/dev/null | sed 's|^|/dev/serial/by-id/|')
    fi

    for p in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$p" ] || continue
        candidates+=("$p")
    done

    if [ ${#candidates[@]} -eq 0 ]; then
        die "找不到串口，请确认板子已插上，或用 -p 指定（例如 -p /dev/ttyACM0）"
    fi
    if [ ${#candidates[@]} -gt 1 ]; then
        warn "检测到多个串口："
        for p in "${candidates[@]}"; do info "  $p"; done
        warn "默认用第一个：${candidates[0]}（要指定就用 -p）"
    fi
    printf '%s' "${candidates[0]}"
}

# 解析 -p 参数，没有就给自动探测结果
resolve_port() {
    local given="$1"
    if [ -n "$given" ]; then
        [ -e "$given" ] || die "指定的串口不存在：$given"
        printf '%s' "$given"
        return 0
    fi
    if [ -n "${COM_PORT:-}" ]; then
        printf '%s' "$COM_PORT"
        return 0
    fi
    detect_port
}

# ---------- 工程信息 ----------
project_info() {
    step "工程信息"
    info "工程目录 : $PROJECT_DIR"
    info "系统     : $(uname -s -m)"
    if [ -f "$PROJECT_DIR/sdkconfig" ]; then
        local target
        target="$(grep -m1 '^CONFIG_IDF_TARGET=' "$PROJECT_DIR/sdkconfig" 2>/dev/null | cut -d'"' -f2)"
        info "目标芯片 : ${target:-?}（来自 sdkconfig）"
    fi
}
