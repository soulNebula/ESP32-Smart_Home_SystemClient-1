# -*- coding: utf-8 -*-
"""一键：编译 -> 烧录 -> 打开串口监视器

这就是"双击那个脚本"背后真正干活的东西。改了代码以后跑一次，剩下的全自动。

用法::

    python tools/sandbox/run.py                 # 编译 + 烧录 + 看串口
    python tools/sandbox/run.py -p COM31        # 指定串口
    python tools/sandbox/run.py --erase         # 先整片擦除再烧（换固件、老出错时用）
    python tools/sandbox/run.py --build-only    # 只编译，不烧录
    python tools/sandbox/run.py --monitor-only  # 只看串口，不编译不烧录
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

from core import (Reporter, Sandbox, SandboxError, ensure_venv,  # noqa: E402
                  init_console, list_serial_ports, open_log, pause_if_needed,
                  print_path_warning)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='ESP32 智能家居 —— 编译、烧录、看串口，一条龙',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='改完代码直接跑这个就行；只改了 WiFi 账号也一样。')
    parser.add_argument('-p', '--port', default='', help='串口名，例如 COM31（不填就自动找）')
    parser.add_argument('--project', default='',
                        help='要构建的工程目录（默认沙箱自己那个工程；可以指向别的 ESP-IDF 工程）')
    parser.add_argument('-b', '--baud', type=int, default=0, help='烧录波特率（一般不用填）')
    parser.add_argument('--erase', action='store_true', help='烧录前整片擦除（NVS 设置会清掉）')
    parser.add_argument('--clean', action='store_true', help='编译前彻底清理（出怪问题时用）')
    parser.add_argument('--build-only', action='store_true', help='只编译')
    parser.add_argument('--monitor-only', action='store_true', help='只看串口，不编译不烧录')
    parser.add_argument('--no-build', action='store_true', help='跳过编译，直接烧上次的固件')
    parser.add_argument('--no-flash', action='store_true', help='跳过烧录')
    parser.add_argument('--no-monitor', action='store_true', help='烧完不开串口监视器')
    parser.add_argument('--seconds', type=float, default=0,
                        help='监视器看够这么多秒自动退出（默认一直看）')
    parser.add_argument('--no-pause', action='store_true', help='结束时不等待回车')
    parser.add_argument('--sandbox', default='', help='指定沙箱目录')
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    init_console()
    args = parse_args(argv)
    sandbox = Sandbox(project=args.project or None, sandbox_dir=args.sandbox or None)
    reporter = open_log(sandbox, 'run')
    started = time.time()
    results: list[tuple[str, str, float]] = []
    code = 0

    try:
        reporter.title('ESP32 智能家居    一键编译烧录')
        reporter.line(f'  工程目录：{sandbox.project}'
                      + ('   ← 外部工程' if sandbox.is_foreign else ''))
        reporter.line(f'  沙箱    ：{sandbox.root}')
        reporter.line(f'  沙箱自带：Python + ESP-IDF {sandbox.idf_version or "?"} + 交叉工具链')
        reporter.hint('这台电脑不需要装 C 编译器、CMake、Ninja、ESP-IDF、Python 中的任何一个')
        print_path_warning(sandbox, reporter)

        do_build = not (args.no_build or args.monitor_only)
        do_flash = not (args.no_flash or args.build_only or args.monitor_only)
        do_monitor = not (args.no_monitor or args.build_only)

        steps = sum((do_build, do_flash, do_monitor))
        reporter.line('')
        reporter.line(f'  一共 {steps} 步：' + ' → '.join(
            [s for s, on in (('编译', do_build), ('烧录', do_flash), ('看串口', do_monitor)) if on]))

        # ---- 1. 沙箱体检 ----
        reporter.step(1, steps + 1, '检查沙箱')
        missing = sandbox.check_essentials()
        if missing:
            reporter.error('沙箱不完整：')
            for item in missing:
                reporter.line(f'           {item}')
            reporter.hint('把整个 .idf-sandbox 目录一起拷过来（不能只拷一部分）')
            reporter.hint('或者运行：python tools/sandbox/prepare.py')
            return 2
        ensure_venv(sandbox, reporter)
        reporter.ok(f'沙箱完好（{sandbox.idf_version}，目标 {sandbox.chip_target}）')

        index = 1

        # ---- 2. 编译 ----
        if do_build:
            index += 1
            reporter.step(index, steps + 1, '编译固件')
            from build import run_build
            code = run_build(sandbox, reporter, clean=args.clean)
            results.append(('编译', '成功' if code == 0 else '失败', 0.0))
            if code != 0:
                reporter.line('')
                reporter.error('编译没过，后面的烧录就先不做了')
                reporter.hint('上面列了可能的原因；实在不行把日志发出来看看')
                return code

        # ---- 3. 烧录 ----
        port = args.port
        if do_flash:
            index += 1
            reporter.step(index, steps + 1, '烧录到板子')
            if not list_serial_ports():
                reporter.warn('现在一个串口都没扫到')
                reporter.hint('板子插上了吗？驱动装了吗？（驱动在 .idf-sandbox\\drivers\\）')
            from flash import run_flash
            flash_code, port = run_flash(sandbox, reporter, port=args.port,
                                         baud=args.baud, erase=args.erase)
            results.append(('烧录', '成功' if flash_code == 0 else '失败', 0.0))
            if flash_code != 0:
                reporter.line('')
                reporter.error('烧录没成功，串口监视器就先不开了')
                return flash_code

        # ---- 4. 串口监视器 ----
        if do_monitor:
            index += 1
            reporter.step(index, steps + 1, '打开串口监视器')
            reporter.line(reporter.c('  板子正在重启，马上就能看到启动日志', 'gray'))
            reporter.hint('Ctrl+C 退出监视器')
            from monitor import run_monitor
            code = run_monitor(sandbox, reporter, port=port, reset=True,
                               seconds=args.seconds)
            results.append(('串口监视器', '已退出', 0.0))

        # ---- 收尾 ----
        reporter.line('')
        reporter.title('完成')
        for name, state, _ in results:
            reporter.line(f'  {name:<10} {state}')
        reporter.line(f'  总用时    {time.time() - started:.1f} 秒')
        reporter.line(f'  日志      {reporter.log_file}')
    except SandboxError as exc:
        reporter.error(str(exc))
        code = 2
    except KeyboardInterrupt:
        reporter.line('')
        reporter.warn('被 Ctrl+C 打断')
        code = 130
    finally:
        reporter.line('')
        reporter.close()
        pause_if_needed(not args.no_pause and code != 0, reporter)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
