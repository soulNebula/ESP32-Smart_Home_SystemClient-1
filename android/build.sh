#!/usr/bin/env bash
# 编译 Android 控制端 APK —— Linux 下的入口
#
# 用法：
#   android/build.sh                        打 debug 包
#   android/build.sh -t assembleRelease     打 release 包
#   android/build.sh --offline              只用本地缓存（不联网）
#   android/build.sh --daemon               保留常驻 Gradle daemon
#   android/build.sh -a -x lint             额外参数原样传给 gradle
#
# 参数：
#   -t, --task      Gradle 任务，默认 assembleDebug
#   -a, --extra     额外参数（可重复）
#   --offline       加 --offline
#   --daemon        不加 --no-daemon（默认会加，见下）
#   -h, --help      帮助
#
# 产物会拷到 android/apk/ 下，命名 SmartHomeBLE-<变体>.apk
#
# 说明：Linux 下源码路径已经是 UTF-8，不需要像 Windows 那样镜像到纯 ASCII 目录

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_RED=$'\033[31m';  C_DIM=$'\033[2m';   C_OFF=$'\033[0m'
else
    C_CYAN=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_DIM=''; C_OFF=''
fi
step() { printf '\n%s=== %s ===%s\n' "$C_CYAN" "$*" "$C_OFF"; }
ok()   { printf '%s  [OK]  %s%s\n' "$C_GREEN" "$*" "$C_OFF"; }
warn() { printf '%s  [WARN] %s%s\n' "$C_YELLOW" "$*" "$C_OFF"; }
err()  { printf '%s  [FAIL] %s%s\n' "$C_RED" "$*" "$C_OFF" >&2; }
info() { printf '%s  %s%s\n' "$C_DIM" "$*" "$C_OFF"; }
die()  { err "$*"; exit 1; }

TASK="assembleDebug"
EXTRA=()
OFFLINE=0
DAEMON=0

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while [ $# -gt 0 ]; do
    case "$1" in
        -t|--task)  TASK="${2:-}"; shift 2 ;;
        -a|--extra) EXTRA+=("${2:-}"); shift 2 ;;
        --offline)  OFFLINE=1; shift ;;
        --daemon)   DAEMON=1; shift ;;
        -h|--help)  usage ;;
        *)          die "未知参数：$1（用 -h 看用法）" ;;
    esac
done

# ---------- JDK ----------
step "查找 JDK"
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ]; then
    ok "JAVA_HOME = $JAVA_HOME"
elif command -v java >/dev/null 2>&1; then
    ok "使用 PATH 里的 java：$(command -v java)"
    JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v java)")")")"
    export JAVA_HOME
    info "推断 JAVA_HOME = $JAVA_HOME"
else
    die "没找到 JDK装一个 JDK 17 或更高版本，或设置 JAVA_HOME"
fi

# ---------- Android SDK ----------
step "查找 Android SDK"
SDK=""
for c in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" \
         "${HOME:-}/Android/Sdk" "${HOME:-}/android-sdk" \
         /usr/lib/android-sdk /opt/android-sdk; do
    [ -n "$c" ] || continue
    if [ -d "$c" ]; then SDK="$c"; break; fi
done
[ -n "$SDK" ] || die "没找到 Android SDK设置 ANDROID_HOME 指向 SDK 目录"
export ANDROID_HOME="$SDK"
export ANDROID_SDK_ROOT="$SDK"
ok "ANDROID_HOME = $SDK"

# ---------- Gradle ----------
step "查找 Gradle"
GRADLE="./gradlew"
if [ -x "$SCRIPT_DIR/gradlew" ]; then
    ok "使用 wrapper：$SCRIPT_DIR/gradlew"
else
    if command -v gradle >/dev/null 2>&1; then
        GRADLE="$(command -v gradle)"
        warn "wrapper 不可用，改用 PATH 里的 gradle：$GRADLE"
        info "补 wrapper：cd android && gradle wrapper"
    else
        die "既没有 gradlew 也没有 gradle装 Gradle 8.9+ 或补上 wrapper"
    fi
fi

# ---------- 编译 ----------
GRADLE_ARGS=(-p "$SCRIPT_DIR" "$TASK" --console=plain -Dfile.encoding=UTF-8)
if [ "$DAEMON" -eq 0 ]; then
    GRADLE_ARGS+=(--no-daemon)
fi
if [ "$OFFLINE" -eq 1 ]; then
    GRADLE_ARGS+=(--offline)
fi
if [ ${#EXTRA[@]} -gt 0 ]; then
    GRADLE_ARGS+=("${EXTRA[@]}")
fi

LOG_FILE="$SCRIPT_DIR/gradle-build.log"
step "Gradle $TASK"
info "命令 : $GRADLE ${GRADLE_ARGS[*]}"
info "日志 : $LOG_FILE"
echo

set +e
if [ "$DAEMON" -eq 1 ]; then
    "$GRADLE" "${GRADLE_ARGS[@]}"
    rc=$?
else
    # 默认不留常驻进程：daemon 会继承 stdout 句柄不释放，管道/CI 里会一直等下去
    "$GRADLE" "${GRADLE_ARGS[@]}" >"$LOG_FILE" 2>&1
    rc=$?
    [ -f "$LOG_FILE" ] && sed 's/^/  /' "$LOG_FILE"
fi
set -e

if [ $rc -ne 0 ]; then
    echo
    err "Gradle 失败（退出码 $rc）"
    info "看日志：$LOG_FILE"
    exit $rc
fi
ok "Gradle $TASK 成功"

# ---------- 收产物 ----------
step "收集 APK"
OUT_DIR="$SCRIPT_DIR/apk"
mkdir -p "$OUT_DIR"
APK_ROOT="$SCRIPT_DIR/app/build/outputs/apk"

if [ ! -d "$APK_ROOT" ]; then
    warn "没找到产物目录：$APK_ROOT"
    exit 0
fi

found=0
while IFS= read -r apk; do
    found=1
    variant="$(basename "$(dirname "$apk")")"
    dest="$OUT_DIR/SmartHomeBLE-$variant.apk"
    cp -f "$apk" "$dest"
    size="$(du -h "$dest" | cut -f1)"
    ok "$dest  ($size)"
done < <(find "$APK_ROOT" -type f -name '*.apk' 2>/dev/null | sort)

if [ "$found" -eq 0 ]; then
    warn "没有找到任何 APK"
else
    echo
    ok "安装： adb install -r $OUT_DIR/SmartHomeBLE-debug.apk"
fi
