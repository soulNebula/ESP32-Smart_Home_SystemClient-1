# -*- coding: utf-8 -*-
"""串口监视器（纯 Python，不需要 PowerShell）

功能对齐原来的 ``monitor.ps1``：

* 自动扫描串口并显示设备名，记住上次用的口
* 彩色显示 ESP32 日志（错误红、警告黄、调试灰）
* 直接敲命令回车就发到板子，Tab 补全命令和设备名
* Ctrl+L 只看自己的打印（过滤掉 I/W/D 日志）
* 全程写进 ``logs/monitor-*.log``

用法::

    python tools/sandbox/monitor.py                # 自动挑串口
    python tools/sandbox/monitor.py -p COM31       # 指定串口
    python tools/sandbox/monitor.py --list         # 只列串口
    python tools/sandbox/monitor.py --send status --seconds 10
"""

from __future__ import annotations

import argparse
import codecs
import sys
import threading
import time
from pathlib import Path

# 支持 Python 3.9 ~ 3.14（见 core.py 里的说明）
if sys.version_info < (3, 9):
    raise SystemExit('[X] 需要 Python 3.9 或更高版本，当前是 ' + sys.version.split()[0]
                     + '\n    请改用沙箱自带的解释器：.idf-sandbox\\python\\python.exe')

sys.path.insert(0, str(Path(__file__).resolve().parent))

from core import (Reporter, Sandbox, choose_port, enable_ansi_colors,  # noqa: E402
                  init_console, list_serial_ports, load_state, pause_if_needed,
                  save_state, show_ports)

BAUD_LIST = (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)
DEFAULT_BAUD = 115200

# 板子固件里的命令（见 main/main.c 的串口调试台）
CMD_LIST = ('help', 'help-voice', 'status', 'on', 'off', 'toggle', 'set', 'color',
            'open', 'close', 'auto', 'cfg', 'say', 'log')
DEV_LIST = ('led_living', 'led_kitchen', 'led_bedroom', 'led_bath', 'fan', 'window',
            'door', 'curtain', 'all')
LOG_LIST = ('quiet', 'all', 'n', 'e', 'w', 'i', 'd', 'v')
AUTO_LIST = ('on', 'off')
CFG_LIST = ('enabled', 'light_on_lux', 'light_off_lux', 'temp_fan_on_c', 'temp_fan_off_c',
            'rain_pct', 'fan_auto_speed', 'auto_light_enable', 'auto_temp_enable',
            'auto_rain_enable', 'auto_window_reopen')
SAY_LIST = ('led_living_on', 'led_living_off', 'led_kitchen_on', 'led_kitchen_off',
            'led_bedroom_on', 'led_bedroom_off', 'led_bath_on', 'led_bath_off',
            'led_all_on', 'led_all_off', 'fan_on', 'fan_off', 'window_open', 'window_close',
            'door_open', 'door_close', 'curtain_open', 'curtain_close',
            'query_temp', 'query_humi', 'query_light', 'query_all', 'query_status',
            'auto_on', 'auto_off')

SUBCOMMANDS = {
    'on': DEV_LIST, 'off': DEV_LIST, 'toggle': DEV_LIST, 'set': DEV_LIST,
    'open': DEV_LIST, 'close': DEV_LIST, 'color': DEV_LIST,
    'auto': AUTO_LIST, 'log': LOG_LIST, 'cfg': CFG_LIST, 'say': SAY_LIST,
}

LOG_LINE_RE = None  # 延迟编译，避免 import re 到顶层


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='ESP32 串口监视器')
    parser.add_argument('-p', '--port', default='', help='串口名，例如 COM31')
    parser.add_argument('-b', '--baud', type=int, default=0, help=f'波特率（默认 {DEFAULT_BAUD}）')
    parser.add_argument('--list', action='store_true', help='只列出串口然后退出')
    parser.add_argument('--reset', dest='reset', action='store_true', default=True,
                        help='连上后复位芯片，从第一行启动日志开始看（默认开）')
    parser.add_argument('--no-reset', dest='reset', action='store_false', help='不复位，直接看')
    parser.add_argument('--seconds', type=float, default=0, help='看够这么多秒就自动退出')
    parser.add_argument('--send', action='append', default=[], help='连上后自动发一条命令（可多次）')
    parser.add_argument('--no-input', action='store_true', help='不接收键盘输入（只记录日志）')
    parser.add_argument('--no-log', action='store_true', help='不写日志文件')
    parser.add_argument('--no-pause', action='store_true', help='结束时不等待回车')
    parser.add_argument('--project', default='', help='日志记到哪个工程下（默认沙箱自己那个）')
    parser.add_argument('--sandbox', default='', help='指定沙箱目录')
    return parser.parse_args(argv)


# ---------------------------------------------------------------------------
# 命令补全
# ---------------------------------------------------------------------------

def complete(text: str) -> str:
    parts = text.split(' ')
    index = len(parts) - 1
    partial = parts[index]
    pool = CMD_LIST if index == 0 else SUBCOMMANDS.get(parts[0], ())
    if not pool:
        return text
    hits = [item for item in pool if item.startswith(partial)]
    if not hits:
        return text
    if len(hits) == 1:
        parts[index] = hits[0]
        print('\r' + ' ' * 80 + '\r  ' + ' '.join(parts), end='', flush=True)
        return ' '.join(parts)

    prefix = hits[0]
    for item in hits:
        while prefix and not item.startswith(prefix):
            prefix = prefix[:-1]
    if len(prefix) > len(partial):
        parts[index] = prefix

    print()
    line = '  '
    for item in sorted(hits):
        if len(line) + len(item) > 74:
            print(line)
            line = '  '
        line += item + '  '
    if line.strip():
        print(line)
    print('  ' + ' '.join(parts), end='', flush=True)
    return ' '.join(parts)


# ---------------------------------------------------------------------------
# 监视器
# ---------------------------------------------------------------------------

class Monitor:
    def __init__(self, reporter: Reporter, port: str, baud: int, reset: bool,
                 interactive: bool, log_path: Path | None, seconds: float,
                 auto_send: tuple[str, ...] = ()):
        self.reporter = reporter
        self.port = port
        self.baud = baud
        self.reset = reset
        self.interactive = interactive
        self.log_path = log_path
        self.seconds = seconds
        self.auto_send = auto_send
        # 图形界面用：可以随时叫停 run()，也不用在屏幕上打欢迎语
        self.announce = True
        self._stop = threading.Event()
        self.filtered = False
        self.typed = ''
        self.pending: list[str] = []
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self.buffer = ''
        self._log = None
        self._port = None

    # -- 颜色 --

    def color_of(self, line: str) -> str:
        stripped = line.strip()
        if stripped.startswith('E (') or ' E (' in line:
            return 'red'
        if stripped.startswith('W (') or ' W (' in line:
            return 'yellow'
        if stripped.startswith('D (') or stripped.startswith('V ('):
            return 'gray'
        if any(k in line for k in ('ESP-ROM', 'boot:', 'rst:0x')):
            return 'cyan'
        if any(k in line for k in ('got ip', 'connected', 'MQTT', 'topics:',
                                   'system ready', 'board init', 'ready')):
            return 'green'
        return ''

    def emit(self, line: str) -> None:
        if self._log:
            try:
                self._log.write(line + '\n')
                self._log.flush()
            except OSError:
                pass
        if self.filtered and _is_log_line(line):
            return
        if self.typed:
            self.pending.append(line)
            return
        self.reporter.line(self.reporter.c(line, self.color_of(line)))

    def flush_pending(self) -> None:
        for line in self.pending:
            self.reporter.line(self.reporter.c(line, self.color_of(line)))
        self.pending.clear()

    # -- 串口 --

    def open(self) -> bool:
        try:
            import serial
        except ImportError:
            self.reporter.error('沙箱里没有 pyserial，无法打开串口')
            return False

        ser = serial.Serial()
        ser.port = self.port
        ser.baudrate = self.baud
        ser.bytesize = serial.EIGHTBITS
        ser.parity = serial.PARITY_NONE
        ser.stopbits = serial.STOPBITS_ONE
        ser.timeout = 0
        ser.write_timeout = 2
        # 打开前先把 DTR/RTS 摆好，免得芯片被误拉进下载模式
        ser.dtr = False
        ser.rts = False
        try:
            ser.open()
        except Exception as exc:  # pyserial 的异常类型比较多，统一兜住
            self.reporter.error(f'打开 {self.port} 失败：{exc}')
            self.reporter.hint('串口被别的程序占着？关掉其它串口工具/Arduino IDE 再试')
            return False

        self._port = ser
        if self.reset:
            self.hard_reset()
        return True

    def hard_reset(self) -> None:
        """拉一下 EN 脚让芯片重启，这样能从头看到启动日志"""
        try:
            self._port.setDTR(False)   # GPIO0 保持高，正常启动
            self._port.setRTS(True)    # EN 拉低 = 复位
            time.sleep(0.12)
            self._port.setRTS(False)
        except Exception:
            pass

    def close(self) -> None:
        if self._port:
            try:
                self._port.close()
            except Exception:
                pass
            self._port = None
        if self._log:
            try:
                self._log.close()
            except Exception:
                pass
            self._log = None

    # -- 键盘 --

    def read_keys(self) -> None:
        if not self.interactive:
            return
        try:
            import msvcrt
        except ImportError:
            self.interactive = False
            return
        try:
            while msvcrt.kbhit():
                ch = msvcrt.getwch()
                if ch in ('\x00', '\xe0'):       # 功能键，吃掉后面那个字节
                    msvcrt.getwch()
                    continue
                self.handle_key(ch)
        except Exception:
            self.interactive = False

    def handle_key(self, ch: str) -> None:
        if ch in ('\r', '\n'):
            print()
            command = self.typed.strip()
            self.typed = ''
            self.flush_pending()
            if command == '?':
                print_help()
            elif command:
                print(self.reporter.c('  > ' + command, 'cyan'))
                if self._log:
                    try:
                        self._log.write('>>> ' + command + '\n')
                    except OSError:
                        pass
                self.send(command)
        elif ch == '\x08':                     # 退格
            if self.typed:
                self.typed = self.typed[:-1]
                print('\b \b', end='', flush=True)
        elif ch == '\t':
            self.typed = complete(self.typed)
        elif ch == '\x0c':                     # Ctrl+L
            self.filtered = not self.filtered
            print()
            self.reporter.warn('只看自己的打印（再按 Ctrl+L 恢复）' if self.filtered
                               else '恢复显示全部日志')
        elif ch == '\x03':                     # Ctrl+C
            raise KeyboardInterrupt
        elif ch >= ' ' and ch != '\x7f':
            self.typed += ch
            print(ch, end='', flush=True)

    def send(self, text: str) -> None:
        try:
            self._port.write((text + '\r\n').encode('utf-8'))
        except Exception as exc:
            self.reporter.error(f'发送失败：{exc}')

    # -- 主循环 --

    def stop(self) -> None:
        """让 run() 尽快返回（图形界面的"停止"按钮用）"""
        self._stop.set()

    @property
    def running(self) -> bool:
        return self._port is not None and not self._stop.is_set()

    def run(self) -> None:
        if self.announce:
            print_help()
        if self.log_path:
            self.reporter.line(self.reporter.c(f'  日志：{self.log_path}', 'gray'))
            print()

        for command in self.auto_send:
            time.sleep(0.6)
            print(self.reporter.c('  > ' + command, 'cyan'))
            self.send(command)

        started = time.time()
        try:
            while not self._stop.is_set():
                if self.seconds and time.time() - started >= self.seconds:
                    break
                data = b''
                try:
                    data = self._port.read(4096)
                except Exception as exc:
                    self.reporter.error(f'读取串口出错：{exc}')
                    break

                if data:
                    self.buffer += self.decoder.decode(data)
                    while '\n' in self.buffer:
                        raw, _, self.buffer = self.buffer.partition('\n')
                        self.emit(raw.rstrip('\r'))
                else:
                    time.sleep(0.005)

                self.read_keys()
        finally:
            if self.buffer:
                self.emit(self.buffer.rstrip('\r'))


def _is_log_line(line: str) -> bool:
    stripped = line.lstrip()
    return len(stripped) > 3 and stripped[0] in 'IWEVD' and stripped[1:3] == ' ('


def print_help() -> None:
    print()
    print('  ' + '=' * 60)
    print('  直接敲命令 + 回车 = 发给板子     Tab = 补全')
    print('  status | on|off <设备> | set <设备> <0-100> | say <命令> | log quiet|all')
    print('  ? 再显示这行提示    Ctrl+L 只看自己的打印    Ctrl+C 退出')
    print('  ' + '=' * 60)
    print()


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------

def run_monitor(sandbox: Sandbox, reporter: Reporter, *, port: str = '', baud: int = 0,
                reset: bool = True, seconds: float = 0.0, send: tuple[str, ...] = (),
                no_log: bool = False, no_input: bool = False) -> int:
    """打开串口监视器（供 run.py 复用），返回退出码"""
    if no_log:
        log_path = None
    else:
        sandbox.log_dir.mkdir(parents=True, exist_ok=True)
        log_path = sandbox.log_dir / f'monitor-{time.strftime("%Y%m%d-%H%M%S")}.log'

    port = choose_port(port)
    if not port:
        reporter.line('')
        reporter.error('没有可用的串口，监视器起不来')
        return 1

    baud = baud or int(load_state().get('baud', DEFAULT_BAUD) or DEFAULT_BAUD)
    interactive = (not no_input) and sys.stdin.isatty()
    save_state({'port': port, 'baud': baud}, sandbox)

    monitor = Monitor(reporter, port, baud, reset, interactive, log_path,
                      seconds, tuple(send))
    if not monitor.open():
        return 1
    try:
        monitor.run()
    except KeyboardInterrupt:
        print()
        reporter.warn('已退出监视器')
    finally:
        monitor.close()
    if log_path:
        reporter.line(f'  串口记录：{log_path}')
    return 0


def main(argv: list[str] | None = None) -> int:
    init_console()
    args = parse_args(argv)
    sandbox = Sandbox(project=args.project or None, sandbox_dir=args.sandbox or None)

    if args.list:
        print('  当前串口：')
        show_ports(list_serial_ports())
        return 0

    color = sys.stdout.isatty() and enable_ansi_colors()
    reporter = Reporter(log_file=None, color=color)
    reporter.title(f'串口监视器     沙箱：{sandbox.root}')

    code = 1
    try:
        code = run_monitor(sandbox, reporter, port=args.port, baud=args.baud,
                           reset=args.reset, seconds=args.seconds, send=tuple(args.send),
                           no_log=args.no_log, no_input=args.no_input)
    except Exception as exc:                      # 串口相关的异常五花八门，别让窗口闪退
        reporter.error(f'监视器出错：{exc}')
        code = 1
    finally:
        reporter.line('')
        reporter.close()
        pause_if_needed(not args.no_pause and code != 0, reporter)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
