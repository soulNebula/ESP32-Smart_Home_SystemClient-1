# -*- coding: utf-8 -*-
"""烧录固件到板子（走沙箱自带的 esptool，不用装任何东西）

用法::

    python .idf-sandbox/tools/flash.py                  # 自动找串口，直接烧
    python .idf-sandbox/tools/flash.py -p COM31         # 指定串口
    python .idf-sandbox/tools/flash.py --erase          # 先整片擦除再烧（换固件/老出错时用）
    python .idf-sandbox/tools/flash.py --monitor        # 烧完顺手打开串口监视器
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

# 支持 Python 3.9 ~ 3.14（见 core.py 里的说明）
if sys.version_info < (3, 9):
    raise SystemExit('[X] 需要 Python 3.9 或更高版本，当前是 ' + sys.version.split()[0]
                     + '\n    请改用沙箱自带的解释器：.idf-sandbox\\python\\python.exe')

sys.path.insert(0, str(Path(__file__).resolve().parent))

from core import (Reporter, Sandbox, SandboxError, choose_port, ensure_venv,  # noqa: E402
                  init_console, load_state, open_log, run_streamed, save_state,
                  pause_if_needed)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='把固件烧进 ESP32')
    parser.add_argument('-p', '--port', default='', help='串口名，例如 COM31（不填就自动找）')
    parser.add_argument('--project', default='', help='要烧录的工程目录（默认烧沙箱自己那个工程）')
    parser.add_argument('-b', '--baud', type=int, default=0, help='烧录波特率（默认最快）')
    parser.add_argument('--erase', action='store_true', help='先整片擦除再烧（NVS 数据也会清掉）')
    parser.add_argument('--monitor', action='store_true', help='烧完直接打开串口监视器')
    parser.add_argument('--build', action='store_true', help='烧之前先编译一遍')
    parser.add_argument('--no-pause', action='store_true', help='结束时不等待回车')
    parser.add_argument('--sandbox', default='', help='指定沙箱目录')
    return parser.parse_args(argv)


def firmware_ready(sandbox: Sandbox) -> bool:
    """看看有没有编译好的固件"""
    return (sandbox.project / 'build' / 'flasher_args.json').is_file()


# 烧录波特率：先快后慢。快的那档绝大多数板子没问题，但**有些 USB 转串口芯片
# （CH343 / CH340 这类）在 460800 下会把数据写坏**，esptool 报
# "MD5 of file does not match data in flash" —— 写进去了，回读一对发现不对。
# 用户看到这种报错完全不知道该干嘛，所以脚本自己降速重试。
BAUD_LADDER = (460800, 115200)

# 这些字样说明"数据没写对"，值得降速重来；别的错误（串口被占用之类）重试没用
WRITE_ERROR_MARKS = (
    'md5 of file does not match',
    'failed to write to target flash',
    'flash write error',
    'invalid header',
    'the chip stopped responding',
)


def looks_like_write_error(tail: list[str]) -> bool:
    text = '\n'.join(tail).lower()
    return any(mark in text for mark in WRITE_ERROR_MARKS)


def run_flash(sandbox: Sandbox, reporter: Reporter, *, port: str = '', baud: int = 0,
              erase: bool = False) -> tuple[int, str]:
    """烧录一次，返回 (退出码, 实际用的串口)

    写坏了会自动降速重试，最后一步还会先整片擦除 —— 见 BAUD_LADDER 的说明。
    """
    if not firmware_ready(sandbox):
        reporter.error('还没编译过，build 目录里没有固件')
        reporter.hint('先点"一键编译烧录"，或者单独跑一次 .idf-sandbox/tools/build.py')
        return 2, ''

    ensure_venv(sandbox, reporter)
    port = choose_port(port)
    if not port:
        reporter.line('')
        reporter.error('找不到串口，没法烧录')
        reporter.hint('板子插好了吗？驱动装了吗？驱动在 .idf-sandbox\\drivers\\ 里')
        return 1, ''

    save_state({'port': port})
    env = sandbox.build_env()

    # 用户自己指定了波特率就听他的，只试一次；否则按阶梯来
    if baud:
        attempts = [(baud, erase)]
    else:
        attempts = [(BAUD_LADDER[0], erase)]
        for speed in BAUD_LADDER[1:]:
            attempts.append((speed, erase))
        # 最后一步：降速 + 整片擦除（把写坏的扇区一起清掉）
        if not erase:
            attempts.append((BAUD_LADDER[-1], True))

    reporter.line(f'  串口：{port}')

    result = None
    for index, (speed, do_erase) in enumerate(attempts):
        if index:
            reporter.line('')
            reporter.warn(f'第 {index + 1} 次尝试：把速度降到 {speed}'
                          + ('，并且先整片擦除' if do_erase else ''))

        if do_erase:
            reporter.info('整片擦除（NVS 里存的设置也会一起清掉）')
            erased = run_streamed(
                sandbox.idf_command(['-p', port, '-b', str(speed), 'erase-flash']),
                env, sandbox.project, reporter)
            if not erased.ok:
                reporter.error('擦除失败，串口是不是被别的程序占着？')
                return erased.code or 1, port
            reporter.ok('擦除完成')

        reporter.info(f'开始烧录（{speed} 波特率），请稍等……')
        result = run_streamed(
            sandbox.idf_command(['-p', port, '-b', str(speed), 'flash']),
            env, sandbox.project, reporter)

        if result.ok:
            reporter.line('')
            reporter.ok(f'烧录成功，用时 {result.seconds:.1f} 秒')
            if index:
                reporter.hint(f'（降到 {speed} 波特率才成功的 —— 这根线/这个转串口芯片'
                              f'在这个速度下更稳，以后就记着它吧）')
            reporter.hint('板子已经自动重启，串口马上就会打印启动日志')
            return 0, port

        if not looks_like_write_error(result.tail):
            break                      # 不是写坏，重试没意义
        if index + 1 < len(attempts):
            reporter.error('数据没写对（esptool 回读校验不通过）')

    reporter.line('')
    reporter.error(f'烧录失败（esptool 退出码 {result.code if result else "?"}）')
    reporter.line('')
    reporter.line(reporter.c('  常见原因：', 'yellow'))
    for hint in diagnose(result.tail if result else []):
        reporter.hint(hint)
    return (result.code if result else 1) or 1, port


def diagnose(tail: list[str]) -> list[str]:
    text = '\n'.join(tail).lower()
    hints: list[str] = []
    rules = (
        ('md5 of file does not match',
         '写进去的数据回读对不上：脚本已经自动降速重试过了还是不行，'
         '换一根**短一点的 USB 数据线**、插电脑**后面**的 USB 口（别用扩展坞/前面板），'
         '板子单独供电再试'),
        ('failed to write to target flash',
         'Flash 写不进去：换 USB 线/口，或者板子供电不足（ESP32-S3 带 PSRAM 比较费电）'),
        ('failed to connect', '连不上芯片：按住板子上的 BOOT 键再点烧录，或者换根 USB 线/换个 USB 口'),
        ('no serial data received', '芯片没响应：确认板子型号是 ESP32-S3，且 BOOT 键的操作对'),
        ('access is denied', '串口被占用：关掉其它串口工具（Arduino IDE、串口助手、另一个监视器窗口）'),
        ('could not open port', '串口打不开：拔了重插，或者换个 USB 口'),
        ('invalid head of packet', '通信不稳：把 USB 线换短一点、换直连电脑的口（别用扩展坞）'),
        ('the chip is not', '芯片型号不对：确认买的是 ESP32-S3'),
        ('timeout', '超时：换个 USB 口或换根数据线；也可能是板子供电不足'),
    )
    for needle, hint in rules:
        if needle in text and hint not in hints:
            hints.append(hint)
    if not hints:
        hints.append('把板子拔下来重新插一次，按住 BOOT 键再点一次烧录')
    hints.append('驱动装了没？看 .idf-sandbox\\drivers\\')
    return hints[:5]


def main(argv: list[str] | None = None) -> int:
    init_console()
    args = parse_args(argv)
    sandbox = Sandbox(project=args.project or None, sandbox_dir=args.sandbox or None)
    reporter = open_log(sandbox, 'flash')
    code = 0
    try:
        reporter.title(f'烧录 ESP32 固件      沙箱：{sandbox.root}')
        reporter.line(f'  工程目录 ：{sandbox.project}'
                      + ('   ← 外部工程' if sandbox.is_foreign else ''))
        reporter.line(f'  固件名称 ：{sandbox.project_name()}')
        if not args.port:
            saved = load_state().get('port', '')
            if saved:
                reporter.line(f'  上次的串口：{saved}')

        if args.build:
            from build import run_build
            code = run_build(sandbox, reporter)
            if code != 0:
                return code

        code, port = run_flash(sandbox, reporter, port=args.port, baud=args.baud,
                               erase=args.erase)
        if code == 0 and args.monitor and port:
            from monitor import run_monitor
            reporter.line('')
            reporter.line(reporter.c('  打开串口监视器（Ctrl+C 退出）', 'cyan'))
            code = run_monitor(sandbox, reporter, port=port)
    except SandboxError as exc:
        reporter.error(str(exc))
        code = 2
    except KeyboardInterrupt:
        reporter.line('')
        reporter.warn('被 Ctrl+C 打断')
        code = 130
    finally:
        reporter.line('')
        reporter.line(f'  日志：{reporter.log_file}')
        reporter.close()
        pause_if_needed(not args.no_pause and code != 0, reporter)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
