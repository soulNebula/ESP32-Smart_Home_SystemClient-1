#!/usr/bin/env bash
# 串口监视 —— Linux 下的入口
#
# 看日志、敲命令（串口调试台默认开启，可直接输入 on led_living 等指令）
# 退出：Ctrl+]
#
# 用法：
#   tools/monitor/monitor.sh                        自动探测串口
#   tools/monitor/monitor.sh -p /dev/ttyACM0        指定串口
#   tools/monitor/monitor.sh --no-log               不落盘日志
#   tools/monitor/monitor.sh --log my.log           指定日志文件
#
# 参数：
#   -p, --port     串口，不写就自动探测
#   -b, --baud     波特率，默认 115200
#   --no-log       不写日志文件
#   --log FILE     日志路径（默认 logs/monitor-<时间>.log）
#   -h, --help     帮助

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
. "$SCRIPT_DIR/../_common.sh"

PORT=""
BAUD="115200"
LOG_FILE=""
USE_LOG=1

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port)  PORT="${2:-}"; shift 2 ;;
        -b|--baud)  BAUD="${2:-}"; shift 2 ;;
        --no-log)   USE_LOG=0; shift ;;
        --log)      LOG_FILE="${2:-}"; shift 2 ;;
        -h|--help)  usage ;;
        *)          die "未知参数：$1（用 -h 看用法）" ;;
    esac
done

prepare_idf
PORT="$(resolve_port "$PORT")"

if [ "$USE_LOG" -eq 1 ] && [ -z "$LOG_FILE" ]; then
    LOG_DIR="$PROJECT_DIR/logs"
    mkdir -p "$LOG_DIR"
    LOG_FILE="$LOG_DIR/monitor-$(date +%Y%m%d-%H%M%S).log"
fi

step "串口监视"
info "串口 : $PORT"
info "波特率 : $BAUD"
if [ -n "$LOG_FILE" ]; then
    info "日志 : $LOG_FILE"
fi
info "退出 : Ctrl+]"
echo

idf_cmd_arr IDF
CMD=("${IDF[@]}" -C "$PROJECT_DIR" -p "$PORT" -b "$BAUD" monitor)

if [ -n "$LOG_FILE" ]; then
    # 同时显示到屏幕和写文件
    set +e
    "${CMD[@]}" 2>&1 | tee "$LOG_FILE"
    rc=${PIPESTATUS[0]}
    set -e
    echo
    ok "日志已保存：$LOG_FILE"
    exit $rc
else
    exec "${CMD[@]}"
fi
