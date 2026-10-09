# -*- coding: utf-8 -*-
"""编译固件（走沙箱，不碰系统环境）

用法::

    python tools/sandbox/build.py            # 增量编译
    python tools/sandbox/build.py --clean    # 先彻底清理再编译
    python tools/sandbox/build.py -v         # 看详细编译命令
    python tools/sandbox/build.py -- size    # "--" 后面的参数原样交给 idf.py

也可以被其它脚本当函数用：``run_build(sandbox, reporter)``
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# 支持 Python 3.9 ~ 3.14（见 core.py 里的说明）
if sys.version_info < (3, 9):
    raise SystemExit('[X] 需要 Python 3.9 或更高版本，当前是 ' + sys.version.split()[0]
                     + '\n    请改用沙箱自带的解释器：.idf-sandbox\\python\\python.exe')

sys.path.insert(0, str(Path(__file__).resolve().parent))

from core import (Reporter, Sandbox, SandboxError, ensure_venv,  # noqa: E402
                  human_size, init_console, open_log, pause_if_needed,
                  print_path_warning, run_streamed)

APP_PARTITION_SIZE = 0x400000  # 兜底值，实际以工程的 sdkconfig + 分区表为准


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='在便携沙箱里编译 ESP32 固件',
        epilog='"--" 后面的参数会原样传给 idf.py，例如：build.py -- size-components')
    parser.add_argument('--clean', action='store_true', help='先 fullclean 再编译（出怪问题时用）')
    parser.add_argument('--task', default='build', help='要跑的 idf.py 子命令（默认 build）')
    parser.add_argument('--project', default='',
                        help='要编译的工程目录（默认编译沙箱自己那个工程）')
    parser.add_argument('-v', '--verbose', action='store_true', help='显示完整编译命令')
    parser.add_argument('-j', '--jobs', type=int, default=0, help='并行任务数（默认交给系统决定）')
    parser.add_argument('--no-pause', action='store_true', help='结束时不等待回车')
    parser.add_argument('--sandbox', default='', help='指定沙箱目录（默认 工程根/.idf-sandbox）')
    parser.add_argument('extra', nargs='*', help='原样传给 idf.py 的额外参数')
    return parser.parse_args(argv)


# ---------------------------------------------------------------------------
# 编译目录的新旧判断
# ---------------------------------------------------------------------------

def build_dir_is_stale(sandbox: Sandbox, reporter: Reporter) -> bool:
    """工程被拷到别的路径（或换了台电脑）后，CMake 缓存里的老路径会让编译失败

    检查三件事，任意一条对不上就重来：

    1. 缓存里记的工程目录不是当前工程目录
    2. 缓存里记的 Python 不是沙箱里的 Python（说明以前用的是系统装的 ESP-IDF）
    3. 缓存里记的 Python 在这台电脑上根本不存在（说明工程被搬过）
    """
    cache = sandbox.project / 'build' / 'CMakeCache.txt'
    if not cache.is_file():
        return False
    try:
        text = cache.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return False

    def cached(key: str) -> str:
        m = re.search(rf'^{re.escape(key)}:[A-Z]+=(.+)$', text, re.M)
        return m.group(1).strip() if m else ''

    def same(a: str, b: str) -> bool:
        return str(Path(a)).lower() == str(Path(b)).lower()

    reason = ''
    home = cached('CMAKE_HOME_DIRECTORY')
    if home and not same(home, sandbox.project):
        reason = f'build 目录是旧工程路径编译的：{home}'
    else:
        recorded_python = cached('PYTHON') or cached('_Python3_EXECUTABLE')
        if recorded_python and not Path(recorded_python).is_file():
            reason = f'build 目录用的 Python 已经不在了：{recorded_python}'
        elif recorded_python and not same(recorded_python, sandbox.venv_python):
            reason = f'build 目录用的不是沙箱里的 Python：{recorded_python}'

    if not reason:
        return False
    reporter.warn(reason)
    reporter.hint('自动清理 build 目录重新生成（第一次会慢一点）')
    return True


# ---------------------------------------------------------------------------
# 编译结果小结
# ---------------------------------------------------------------------------

def firmware_summary(sandbox: Sandbox) -> list[str]:
    """给一句"固件多大、还剩多少地方"的结论"""
    lines: list[str] = []
    try:
        desc = json.loads((sandbox.project / 'build' / 'project_description.json')
                          .read_text(encoding='utf-8'))
        app_bin = Path(desc.get('app_elf', '')).with_suffix('.bin')
        if app_bin.is_file():
            size = app_bin.stat().st_size
            # 分区大小按"被编译的那个工程"算，不是按沙箱自己那个工程
            total = sandbox.app_partition_size
            percent = size / total * 100
            lines.append(f'固件：{app_bin.name}  {human_size(size)}  '
                         f'（占 app 分区 {percent:.1f}%，余 {human_size(total - size)}）')
    except (OSError, ValueError, KeyError, ZeroDivisionError):
        pass
    return lines


def diagnose(tail: list[str]) -> list[str]:
    """从编译输出尾部猜几个常见原因"""
    text = '\n'.join(tail)
    lowered = text.lower()
    hints: list[str] = []
    rules = (
        ('no such file or directory', '有文件没找到：多半是路径被动过，试一次 --clean 重新编译'),
        ('permission denied', '文件被占用或没权限：关掉正在看串口的窗口、杀毒软件放行一下'),
        ('undefined reference', '链接错误：某个函数只声明没实现，或者组件没写进 CMakeLists 的 REQUIRES'),
        ('fatal error:', 'C 编译错误：往上找第一条 fatal error，看它的文件名和行号'),
        ('component directory', '组件问题：确认 managed_components/ 跟着工程一起拷过来了'),
        ('not found in the registry', '组件管理器想联网：managed_components/ 没拷全'),
        ('no space left', '磁盘满了'),
        ('cannot create temporary file', '临时目录不能写：关掉杀毒软件的"文件夹保护"再试'),
    )
    for needle, hint in rules:
        if needle in lowered and hint not in hints:
            hints.append(hint)
    if not hints:
        hints.append('往上翻，找到第一处写着 error 的地方，那一行才是真正的原因')
    hints.append('改了 WiFi 账号？见 components/App/wifi_config.h')
    return hints[:6]


# ---------------------------------------------------------------------------
# 对外函数
# ---------------------------------------------------------------------------

def component_manager_needed(sandbox: Sandbox, reporter: Reporter) -> bool:
    """判断这个工程要不要开组件管理器

    沙箱默认把它关掉（离线、启动快），前提是依赖已经在 managed_components/ 里。
    换成别的工程时：如果它声明了 idf_component.yml 却一个依赖都没下过，
    那就只能把管理器打开让它自己下（要联网）。
    """
    if not sandbox.needs_component_manager():
        return False
    reporter.warn('这个工程声明了组件依赖，但 managed_components/ 是空的')
    reporter.hint('临时打开官方组件管理器去下载（第一次要联网，之后就离线了）')
    return True


def component_dir_args(sandbox: Sandbox, use_manager: bool) -> list[str]:
    """把目标工程的 managed_components/ 挂进组件搜索路径

    我们自己的工程在 CMakeLists.txt 里做了这件事；别的工程没做过，
    又不好去改人家的源码，所以用 -D 从命令行挂上去。
    """
    if use_manager or not sandbox.managed_components or not sandbox.is_foreign:
        return []
    return [f'-DEXTRA_COMPONENT_DIRS={sandbox.managed_components}']


def run_build(sandbox: Sandbox, reporter: Reporter, *, clean: bool = False,
              verbose: bool = False, jobs: int = 0, task: str = 'build',
              extra: tuple[str, ...] = ()) -> int:
    """编译一次，返回退出码（0 = 成功）"""
    missing = sandbox.check_essentials()
    if missing:
        reporter.error('沙箱不完整，缺少下面这些文件：')
        for item in missing:
            reporter.line(f'           {item}')
        reporter.hint('请把整个 .idf-sandbox 目录一起拷过来（不能只拷一部分）')
        reporter.hint('或者运行：python tools/sandbox/prepare.py')
        return 2

    problems = sandbox.project_problems()
    if problems:
        reporter.error(f'这个目录不像是能编译的 ESP-IDF 工程：{sandbox.project}')
        for item in problems:
            reporter.hint(item)
        return 2

    ensure_venv(sandbox, reporter)
    use_manager = component_manager_needed(sandbox, reporter)
    env = sandbox.build_env(component_manager=use_manager)
    if build_dir_is_stale(sandbox, reporter):
        clean = True

    if clean:
        reporter.info('清理旧的编译产物……')
        result = run_streamed(sandbox.idf_command(['fullclean']), env, sandbox.project, reporter)
        if not result.ok:
            reporter.warn('fullclean 没成功；不行就手动删掉 build 目录，不影响继续')

    idf_args = component_dir_args(sandbox, use_manager) + [task]
    if verbose:
        idf_args.append('-v')
    if jobs:
        idf_args += ['-j', str(jobs)]
    idf_args += [a for a in extra if a != '--']

    reporter.info(f'工程：{sandbox.project_name()}  ({sandbox.project})')
    reporter.info('idf.py ' + ' '.join(idf_args))
    result = run_streamed(sandbox.idf_command(idf_args), env, sandbox.project, reporter)

    reporter.line('')
    if result.ok:
        reporter.ok(f'编译成功，用时 {result.seconds:.1f} 秒')
        for line in firmware_summary(sandbox):
            reporter.line('         ' + line)
        return 0

    reporter.error(f'编译失败（idf.py 退出码 {result.code}）')
    reporter.line('')
    reporter.line(reporter.c('  可能的原因：', 'yellow'))
    for hint in diagnose(result.tail):
        reporter.hint(hint)
    return result.code or 1


def main(argv: list[str] | None = None) -> int:
    init_console()
    args = parse_args(argv)
    sandbox = Sandbox(project=args.project or None, sandbox_dir=args.sandbox or None)
    reporter = open_log(sandbox, 'build')
    code = 0
    try:
        reporter.title(f'编译 ESP32 固件      沙箱：{sandbox.root}')
        reporter.line(f'  工程目录 ：{sandbox.project}'
                      + ('   ← 外部工程' if sandbox.is_foreign else ''))
        reporter.line(f'  ESP-IDF  ：{sandbox.idf_version or "?"}  ({sandbox.idf_dir})')
        reporter.line(f'  目标芯片 ：{sandbox.chip_target}')
        print_path_warning(sandbox, reporter)
        code = run_build(sandbox, reporter, clean=args.clean, verbose=args.verbose,
                         jobs=args.jobs, task=args.task, extra=tuple(args.extra))
        reporter.line('')
        reporter.line(f'  日志：{reporter.log_file}')
    except SandboxError as exc:
        reporter.error(str(exc))
        code = 2
    except KeyboardInterrupt:
        reporter.line('')
        reporter.warn('被 Ctrl+C 打断')
        code = 130
    finally:
        reporter.close()
        pause_if_needed(not args.no_pause and code != 0, reporter)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
