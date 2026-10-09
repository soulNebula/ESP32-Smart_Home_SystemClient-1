# -*- coding: utf-8 -*-
"""环境体检：一条命令看清沙箱到底好不好

客户机出问题时，让对方双击"环境体检.cmd"，把屏幕上的内容截图发过来就行。

用法::

    python tools/sandbox/doctor.py            # 体检
    python tools/sandbox/doctor.py --ports    # 顺便列串口
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# 支持 Python 3.9 ~ 3.14（见 core.py 里的说明）
if sys.version_info < (3, 9):
    raise SystemExit('[X] 需要 Python 3.9 或更高版本，当前是 ' + sys.version.split()[0]
                     + '\n    请改用沙箱自带的解释器：.idf-sandbox\\python\\python.exe')

sys.path.insert(0, str(Path(__file__).resolve().parent))

from core import (NO_WINDOW, Reporter, Sandbox, dir_size, ensure_venv,  # noqa: E402
                  human_size, init_console, is_ascii, list_serial_ports, open_log,
                  pause_if_needed, show_ports)

OK, BAD, WARN = 'ok', 'bad', 'warn'


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='沙箱环境体检')
    parser.add_argument('--ports', action='store_true', help='列出所有串口和驱动信息')
    parser.add_argument('--no-pause', action='store_true', help='结束时不等待回车')
    parser.add_argument('--project', default='', help='体检哪个工程（默认沙箱自己那个）')
    parser.add_argument('--sandbox', default='', help='指定沙箱目录')
    return parser.parse_args(argv)


def run_tool(cmd: list[str], env: dict, timeout: int = 30) -> str:
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8',
                             errors='replace', env=env, timeout=timeout,
                             creationflags=NO_WINDOW)
        text = (out.stdout or out.stderr or '').strip().splitlines()
        return text[0].strip() if text else ''
    except Exception as exc:
        return f'({exc})'


def main(argv: list[str] | None = None) -> int:
    init_console()
    args = parse_args(argv)
    sandbox = Sandbox(project=args.project or None, sandbox_dir=args.sandbox or None)
    reporter = open_log(sandbox, 'doctor')
    problems: list[str] = []

    def item(status: str, label: str, value: str = '') -> None:
        mark, color = {'ok': ('[OK]  ', 'green'), 'warn': ('[注意]', 'yellow'),
                       'bad': ('[缺]  ', 'red')}[status]
        reporter.line(f'  {reporter.c(mark, color)} {label:<22} {value}')
        if status == 'bad':
            problems.append(label)

    try:
        reporter.title('沙箱环境体检')

        # ---- 路径 ----
        reporter.banner('路径')
        item(OK, '工程目录', str(sandbox.project))
        exists = sandbox.root.is_dir()
        item(OK if exists else BAD, '沙箱目录', str(sandbox.root))
        ascii_ok = is_ascii(str(sandbox.project)) and is_ascii(str(sandbox.root))
        item(OK if ascii_ok else WARN, '路径是否纯英文',
             '是' if ascii_ok else '否（可能影响交叉编译器，建议挪到 D:\\esp32_smart_home）')
        if exists:
            reporter.hint(f'沙箱体积 {human_size(dir_size(sandbox.root))}'
                          f'（整个文件夹拷走就是完整环境）')

        # ---- Python ----
        reporter.banner('Python')
        version = sys.version_info
        supported = (3, 9) <= version[:2] <= (3, 14)
        item(OK if supported else WARN, '跑本脚本的 Python',
             f'{sys.version.split()[0]}  ({sys.executable})')
        item(OK, '脚本兼容范围', 'Python 3.9 ~ 3.14')
        item(OK if sandbox.python_exe.is_file() else BAD, '沙箱便携 Python',
             str(sandbox.python_exe) if sandbox.python_exe.is_file() else '不见了')
        if sandbox.python_exe.is_file():
            env = sandbox.build_env()
            reporter.line(f'         版本：{run_tool([str(sandbox.python_exe), "-V"], env)}')
            reporter.hint('编译时真正干活的是沙箱这个 Python，和上面那个无关')
            try:
                ensure_venv(sandbox, reporter)
                item(OK, '虚拟环境', str(sandbox.venv_dir))
                missing_deps = check_python_deps(sandbox, env)
                item(OK if not missing_deps else BAD, 'Python 依赖包',
                     '齐全' if not missing_deps else '缺：' + ', '.join(missing_deps))
            except Exception as exc:
                item(BAD, '虚拟环境', str(exc))

        # ---- ESP-IDF ----
        reporter.banner('ESP-IDF')
        item(OK if sandbox.idf_py.is_file() else BAD, 'idf.py',
             str(sandbox.idf_py) if sandbox.idf_py.is_file() else '不见了')
        item(OK if sandbox.idf_version else BAD, 'IDF 版本', sandbox.idf_version or '读不到')
        item(OK, '目标芯片', sandbox.chip_target)
        sdkconfig = sandbox.project / 'sdkconfig'
        item(OK if sdkconfig.is_file() else WARN, 'sdkconfig',
             human_size(sdkconfig.stat().st_size) if sdkconfig.is_file() else '没有（会用 sdkconfig.defaults 重新生成）')

        # ---- 工具链 ----
        reporter.banner('工具链')
        env = sandbox.build_env()
        from core import REQUIRED_TOOLS
        for name in REQUIRED_TOOLS:
            path = sandbox.tool_path(name)
            item(OK if path else BAD, name, path.name if path else '缺少')
        optional_present = [n for n in ('ccache', 'riscv32-esp-elf', 'openocd-esp32',
                                        'xtensa-esp-elf-gdb')
                            if sandbox.tool_path(n)]
        reporter.hint('未打包的可选工具：' + ', '.join(
            n for n in ('riscv32-esp-elf', 'openocd-esp32', 'xtensa-esp-elf-gdb')
            if n not in optional_present) + '（编译烧录用不到）')

        gcc = sandbox.tool_path('xtensa-esp-elf')
        if gcc:
            gcc_exe = gcc / 'xtensa-esp-elf' / 'bin' / 'xtensa-esp32s3-elf-gcc.exe'
            item(OK if gcc_exe.is_file() else BAD, 'xtensa gcc',
                 run_tool([str(gcc_exe), '--version'], env) if gcc_exe.is_file() else '缺少')
        cmake = sandbox.tool_path('cmake')
        if cmake:
            item(OK, 'cmake', run_tool([str(cmake / 'bin' / 'cmake.exe'), '--version'], env))
        ninja = sandbox.tool_path('ninja')
        if ninja:
            item(OK, 'ninja', run_tool([str(ninja / 'ninja.exe'), '--version'], env))

        # ---- 工程依赖 ----
        reporter.banner('工程')
        managed = sandbox.project / 'managed_components'
        comps = sorted(p.name for p in managed.iterdir()) if managed.is_dir() else []
        item(OK if comps else BAD, 'managed_components',
             ', '.join(comps) if comps else '没有（esp-sr 语音组件缺了会编译失败）')
        parts = sandbox.project / 'partitions-16MiB.csv'
        item(OK if parts.is_file() else WARN, '分区表', parts.name if parts.is_file() else '没有')
        build_bin = sandbox.project / 'build' / 'project_description.json'
        item(OK if build_bin.is_file() else WARN, '上次编译',
             '有' if build_bin.is_file() else '还没编译过')

        # ---- 串口 ----
        reporter.banner('串口')
        ports = list_serial_ports()
        real_ports = [p for p in ports if not p.is_virtual]
        if ports:
            show_ports(ports)
        if real_ports:
            item(OK, '可用串口', f'{len(real_ports)} 个 USB 串口')
        elif ports:
            item(WARN, '可用串口', '只看到虚拟串口，没看到开发板')
            reporter.hint('板子插上了吗？换根 USB 数据线 / 换个 USB 口试试')
        else:
            item(WARN, '可用串口', '一个都没有（板子没插 / 驱动没装）')
            drivers = sandbox.drivers_dir
            if drivers.is_dir():
                reporter.hint(f'驱动安装包在：{drivers}')
                for child in sorted(drivers.iterdir())[:6]:
                    reporter.hint(f'  - {child.name}')

        # ---- 系统 ----
        reporter.banner('电脑')
        item(OK, '操作系统', f'{platform.system()} {platform.release()} {platform.version()}')
        item(OK, 'CPU 核心', str(os.cpu_count() or '?'))
        try:
            usage = shutil.disk_usage(str(sandbox.project))
            item(OK if usage.free > 2 * 1024 ** 3 else WARN, '剩余磁盘空间',
                 human_size(usage.free))
        except OSError:
            pass

        # ---- 结论 ----
        reporter.line('')
        reporter.title('体检结论')
        if problems:
            reporter.error('有问题的地方：' + '、'.join(problems))
            reporter.hint('缺文件的话，把整个 .idf-sandbox 目录重新拷一遍')
            reporter.hint('或者运行：python tools/sandbox/prepare.py')
            code = 1
        else:
            reporter.ok('沙箱完全正常，点"一键编译烧录"就能干活')
            code = 0
    except Exception as exc:
        reporter.error(f'体检过程出错：{exc}')
        code = 1
    finally:
        reporter.line('')
        reporter.line(f'  完整报告：{reporter.log_file}')
        reporter.close()
        pause_if_needed(not args.no_pause, reporter)
    return code


def check_python_deps(sandbox: Sandbox, env: dict) -> list[str]:
    """确认 idf.py 要用的几个包都在（少了会直接跑不起来）"""
    modules = ('click', 'pyserial', 'cryptography', 'esptool', 'esp_idf_monitor',
               'esp_idf_kconfig', 'pyelftools', 'rich')
    probe = ';'.join([
        'import importlib,sys',
        'missing=[m for m in %r if importlib.util.find_spec(m) is None]' % (modules,),
        'print(",".join(missing))',
    ])
    try:
        out = subprocess.run([str(sandbox.venv_python), '-c', probe],
                             capture_output=True, text=True, encoding='utf-8',
                             errors='replace', env=env, timeout=60,
                             creationflags=NO_WINDOW)
        text = (out.stdout or '').strip()
        return [m for m in text.split(',') if m]
    except Exception:
        return []


if __name__ == '__main__':
    raise SystemExit(main())
