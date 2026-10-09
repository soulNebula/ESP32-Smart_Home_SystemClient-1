# -*- coding: utf-8 -*-
"""沙箱准备 / 修复

正常情况下用不到这个脚本：沙箱 (.idf-sandbox) 是随工程一起拷过来的，开箱即用。
只有在下面这些情况下才需要它：

* 沙箱只拷了一半，或者被杀毒软件删掉了几个文件
* 想在一台新的开发机上重新攒一套沙箱
* 想升级 / 换一个 ESP-IDF 版本

三种用法::

    python tools/sandbox/prepare.py                      # 只体检，看缺什么
    python tools/sandbox/prepare.py --repair             # 从本机已装的 ESP-IDF 补齐
    python tools/sandbox/prepare.py --download           # 缺什么就从网上下什么

``--repair`` 会自己去找本机的 ESP-IDF：

* 官方安装器装的（``C:\\Espressif`` / ``E:\\Espressif`` / ``%IDF_TOOLS_PATH%`` 等）
* PlatformIO 的包目录（``~/.platformio/packages``）
* 环境变量 ``IDF_PATH`` 指的目录

``--download`` 走官方渠道：ESP-IDF 源码走 GitHub release 压缩包，
工具链走 ``idf_tools.py install``（Espressif 官方下载器，支持国内镜像）。
注意：便携 Python 没法从网上下——它必须跟着沙箱一起拷过来。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

# 支持 Python 3.9 ~ 3.14（见 core.py 里的说明）。
# 这个脚本最需要向下兼容：沙箱 Python 没了的机器，只能靠系统里的 Python 来修沙箱。
if sys.version_info < (3, 9):
    raise SystemExit('[X] 需要 Python 3.9 或更高版本，当前是 ' + sys.version.split()[0]
                     + '\n    请改用沙箱自带的解释器：.idf-sandbox\\python\\python.exe')

sys.path.insert(0, str(Path(__file__).resolve().parent))

from core import (NO_WINDOW, Reporter, Sandbox, init_console, human_size,  # noqa: E402
                  dir_size, open_log, pause_if_needed)

IDF_VERSION = '5.4.4'
IDF_ZIP_URLS = (
    f'https://dl.espressif.com/github_assets/espressif/esp-idf/releases/download/v{IDF_VERSION}/esp-idf-v{IDF_VERSION}.zip',
    f'https://github.com/espressif/esp-idf/releases/download/v{IDF_VERSION}/esp-idf-v{IDF_VERSION}.zip',
)

# 拷贝 ESP-IDF 时丢掉这些（省 190 MB，编译一点不影响）
IDF_SKIP_DIRS = ('.git', 'docs', 'examples', '.github', '.gitlab')

# PlatformIO 的包名 -> 沙箱里的工具名
PIO_TOOL_MAP = {
    'toolchain-xtensa-esp-elf': 'xtensa-esp-elf',
    'tool-cmake': 'cmake',
    'tool-ninja': 'ninja',
    'tool-esptoolpy': None,      # esptool 在 Python 环境里，不用单独拷
}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='准备 / 修复便携沙箱')
    parser.add_argument('--repair', action='store_true', help='从本机已装的 ESP-IDF 补齐缺的东西')
    parser.add_argument('--download', action='store_true', help='缺什么就从官方渠道下载什么')
    parser.add_argument('--from', dest='source', default='',
                        help='指定来源目录（官方 ESP-IDF 安装根目录，或 PlatformIO 的 packages 目录）')
    parser.add_argument('--force', action='store_true', help='已经有的也重新拷一遍')
    parser.add_argument('--list-sources', action='store_true', help='只列出找到的本机来源')
    parser.add_argument('--no-pause', action='store_true', help='结束时不等待回车')
    parser.add_argument('--sandbox', default='', help='指定沙箱目录')
    return parser.parse_args(argv)


# ---------------------------------------------------------------------------
# 找一个本机的 ESP-IDF
# ---------------------------------------------------------------------------

class Source:
    def __init__(self, label: str, idf_dir: Path | None = None,
                 tools_root: Path | None = None, python_dir: Path | None = None):
        self.label = label
        self.idf_dir = idf_dir
        self.tools_root = tools_root
        self.python_dir = python_dir

    def __repr__(self) -> str:
        bits = [self.label]
        if self.idf_dir:
            bits.append(f'idf={self.idf_dir}')
        if self.tools_root:
            bits.append(f'tools={self.tools_root}')
        if self.python_dir:
            bits.append(f'python={self.python_dir}')
        return '  '.join(bits)


def looks_like_idf(path: Path) -> bool:
    return (path / 'tools' / 'idf.py').is_file()


def find_espressif_python(root: Path) -> Path | None:
    """官方安装器把便携 Python 放在 tools/idf-python/<版本>/"""
    base = root / 'tools' / 'idf-python'
    if not base.is_dir():
        return None
    for child in sorted(base.iterdir(), reverse=True):
        if (child / 'python.exe').is_file():
            return child
    return None


def detect_sources() -> list[Source]:
    sources: list[Source] = []

    def add_espressif(root: Path, label: str) -> None:
        if not root.is_dir():
            return
        idf = None
        frameworks = root / 'frameworks'
        if frameworks.is_dir():
            for child in sorted(frameworks.iterdir(), reverse=True):
                if looks_like_idf(child):
                    idf = child
                    break
        if idf is None and looks_like_idf(root):
            idf = root
        tools = root / 'tools' if (root / 'tools').is_dir() else None
        if idf or tools:
            sources.append(Source(label, idf, tools, find_espressif_python(root)))

    # 1) 环境变量优先
    for key, label in (('IDF_TOOLS_PATH', '环境变量 IDF_TOOLS_PATH'),):
        value = os.environ.get(key, '')
        if value:
            add_espressif(Path(value), label)
    idf_env = os.environ.get('IDF_PATH', '')
    if idf_env and looks_like_idf(Path(idf_env)):
        sources.append(Source('环境变量 IDF_PATH', Path(idf_env)))

    # 2) 常见安装位置
    roots = [Path('C:/Espressif'), Path('D:/Espressif'), Path('E:/Espressif'),
             Path('F:/Espressif'), Path.home() / 'esp', Path.home() / '.espressif']
    for root in roots:
        add_espressif(root, f'官方安装目录 {root}')

    # 3) PlatformIO 的包目录
    for pio in (Path.home() / '.platformio' / 'packages',
                Path(os.environ.get('PLATFORMIO_CORE_DIR', '')) / 'packages'
                if os.environ.get('PLATFORMIO_CORE_DIR') else None):
        if not pio or not pio.is_dir():
            continue
        idf = pio / 'framework-espidf'
        if looks_like_idf(idf):
            sources.append(Source(f'PlatformIO {pio}', idf, pio, None))

    return sources


# ---------------------------------------------------------------------------
# 拷贝
# ---------------------------------------------------------------------------

def copy_tree(src: Path, dst: Path, reporter: Reporter, skip_dirs: tuple[str, ...] = ()) -> bool:
    """大目录用 robocopy 拷（比 Python 自己拷快得多），没有就退回 shutil"""
    dst.mkdir(parents=True, exist_ok=True)
    if os.name == 'nt' and shutil.which('robocopy'):
        cmd = ['robocopy', str(src), str(dst), '/E', '/MT:16', '/NFL', '/NDL',
               '/NJH', '/NJS', '/NP']
        for name in skip_dirs:
            cmd += ['/XD', str(src / name)]
        result = subprocess.run(cmd, capture_output=True, text=True,
                                encoding='utf-8', errors='replace',
                                creationflags=NO_WINDOW)
        # robocopy 的退出码 < 8 都算成功
        if result.returncode < 8:
            return True
        reporter.warn(f'robocopy 退出码 {result.returncode}')
    try:
        shutil.copytree(src, dst, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns(*skip_dirs) if skip_dirs else None)
        return True
    except OSError as exc:
        reporter.error(f'拷贝失败：{exc}')
        return False


def copy_tool(src: Path, dst: Path, reporter: Reporter, skip_dirs=()) -> bool:
    if dst.is_dir() and any(dst.iterdir()):
        return True
    return copy_tree(src, dst, reporter, skip_dirs)


# ---------------------------------------------------------------------------
# 各项安装
# ---------------------------------------------------------------------------

def install_python(sandbox: Sandbox, source: Source, reporter: Reporter, force: bool) -> bool:
    if sandbox.python_exe.is_file() and not force:
        return True
    if not source.python_dir:
        reporter.warn(f'{source.label} 里没有便携 Python')
        return False
    reporter.info(f'拷贝便携 Python：{source.python_dir} -> {sandbox.python_dir}')
    # 先删掉不完整的旧目录，免得混在一起
    if sandbox.python_dir.exists() and force:
        shutil.rmtree(sandbox.python_dir, ignore_errors=True)
    return copy_tree(source.python_dir, sandbox.python_dir, reporter)


def install_python_deps(sandbox: Sandbox, source: Source, reporter: Reporter) -> bool:
    """Python 依赖包：官方安装器的 python_env 里已经有了，直接搬过来"""
    probe = ['click', 'esptool', 'esp_idf_monitor', 'esp_idf_kconfig', 'pyserial']
    env = sandbox.build_env()
    result = subprocess.run(
        [str(sandbox.python_exe), '-c',
         'import importlib.util as u;print(",".join(m for m in %r if u.find_spec(m) is None))' % (probe,)],
        capture_output=True, text=True, encoding='utf-8', errors='replace', env=env,
        creationflags=NO_WINDOW)
    missing = [m for m in (result.stdout or '').strip().split(',') if m]
    if not missing:
        return True
    reporter.info(f'沙箱 Python 缺依赖：{", ".join(missing)}')

    if source.tools_root:
        root = source.tools_root.parent
        envs = root / 'python_env'
        if envs.is_dir():
            for child in sorted(envs.iterdir(), reverse=True):
                site = child / 'Lib' / 'site-packages'
                if site.is_dir():
                    reporter.info(f'从 {site} 搬依赖包')
                    return copy_tree(site, sandbox.python_dir / 'Lib' / 'site-packages',
                                     reporter)
    reporter.warn('没找到现成的依赖包；如果这台电脑能上网，可以试试：')
    reporter.hint(f'"{sandbox.python_exe}" -m pip install -r "{sandbox.idf_dir}\\tools\\requirements\\requirements.core.txt"')
    return False


def install_idf(sandbox: Sandbox, source: Source, reporter: Reporter, force: bool) -> bool:
    if sandbox.idf_py.is_file() and not force:
        return True
    if not source.idf_dir:
        reporter.warn(f'{source.label} 里没有 ESP-IDF 源码')
        return False
    reporter.info(f'拷贝 ESP-IDF：{source.idf_dir} -> {sandbox.idf_dir}')
    if sandbox.idf_dir.exists() and force:
        shutil.rmtree(sandbox.idf_dir, ignore_errors=True)
    return copy_tree(source.idf_dir, sandbox.idf_dir, reporter, IDF_SKIP_DIRS)


def install_tools(sandbox: Sandbox, source: Source, reporter: Reporter, force: bool) -> bool:
    if not source.tools_root:
        reporter.warn(f'{source.label} 里没有工具链目录')
        return False
    ok = True
    for name in ('xtensa-esp-elf', 'cmake', 'ninja', 'esp-rom-elfs', 'idf-exe'):
        target = sandbox.idf_tools_dir / 'tools' / name
        if sandbox.tool_path(name) and not force:
            continue
        wanted = sandbox.tool_version(name)
        src = source.tools_root / name / wanted if wanted else None
        if not src or not src.is_dir():
            # PlatformIO 那种目录名不一样，单独映射一下
            mapped = next((p for p, v in PIO_TOOL_MAP.items() if v == name), None)
            candidate = source.tools_root / mapped if mapped else None
            if candidate and candidate.is_dir():
                src = candidate
                wanted = wanted or 'local'
            else:
                reporter.warn(f'来源里找不到工具 {name}')
                ok = False
                continue
        reporter.info(f'拷贝工具链 {name}（{human_size(dir_size(src))}）')
        if not copy_tool(src, target / (wanted or 'local'), reporter):
            ok = False
    return ok


# ---------------------------------------------------------------------------
# 从网上下
# ---------------------------------------------------------------------------

def download_idf(sandbox: Sandbox, reporter: Reporter) -> bool:
    dest = sandbox.idf_dir
    url = ''
    for candidate in IDF_ZIP_URLS:
        try:
            reporter.info(f'试探下载地址：{candidate}')
            with urllib.request.urlopen(candidate, timeout=20) as response:
                if response.status == 200:
                    url = candidate
                    break
        except Exception as exc:
            reporter.warn(f'连不上（{exc}）')
    if not url:
        reporter.error('两个下载地址都连不上，检查网络')
        return False

    archive = sandbox.root / 'download' / f'esp-idf-v{IDF_VERSION}.zip'
    archive.parent.mkdir(parents=True, exist_ok=True)
    reporter.info(f'下载 ESP-IDF {IDF_VERSION}（约 250 MB，慢慢等）')
    try:
        _download_with_progress(url, archive, reporter)
    except Exception as exc:
        reporter.error(f'下载失败：{exc}')
        return False

    reporter.info('解压……')
    try:
        with zipfile.ZipFile(archive) as zf:
            names = zf.namelist()
            root = names[0].split('/')[0] if names else ''
            for member in names:
                target = Path(member)
                parts = target.parts[1:] if root and target.parts[0] == root else target.parts
                if not parts:
                    continue
                if parts[0] in IDF_SKIP_DIRS:
                    continue
                out = dest.joinpath(*parts)
                if member.endswith('/'):
                    out.mkdir(parents=True, exist_ok=True)
                else:
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with zf.open(member) as src, open(out, 'wb') as dst:
                        shutil.copyfileobj(src, dst, 1024 * 256)
    except Exception as exc:
        reporter.error(f'解压失败：{exc}')
        return False
    archive.unlink(missing_ok=True)
    return True


def _download_with_progress(url: str, dest: Path, reporter: Reporter) -> None:
    with urllib.request.urlopen(url, timeout=60) as response, open(dest, 'wb') as out:
        total = int(response.headers.get('Content-Length') or 0)
        done = 0
        last = 0.0
        import time as _time
        while True:
            chunk = response.read(1024 * 256)
            if not chunk:
                break
            out.write(chunk)
            done += len(chunk)
            now = _time.time()
            if now - last > 2.0:
                last = now
                if total:
                    reporter.raw(f'\r  已下载 {human_size(done)} / {human_size(total)} '
                                 f'({done * 100 // total}%)')
                else:
                    reporter.raw(f'\r  已下载 {human_size(done)}')
        reporter.raw('\r' + ' ' * 60 + '\r')


def download_tools(sandbox: Sandbox, reporter: Reporter) -> bool:
    """用官方下载器装这一套平台对应的工具链

    ``idf_tools.py`` 会按**当前操作系统**挑包，所以：

    * 在 Windows 上跑，装的是 ``idf_tools/``（*.exe）
    * 在 Linux 上跑，装的是 ``idf_tools-linux/``（ELF）

    Linux 上不一定有沙箱自带的 Python，那就借系统那个来跑下载器。
    """
    if not sandbox.idf_py.is_file():
        reporter.error('得先有 ESP-IDF 才能下载工具链')
        return False
    python = sandbox.python_exe if sandbox.python_exe.is_file() else Path(sys.executable)
    if not python.is_file():
        reporter.error('找不到可用的 Python 来解释 idf_tools.py')
        return False

    env = sandbox.build_env()
    tools = ['xtensa-esp-elf', 'cmake', 'ninja', 'esp-rom-elfs']
    if sandbox.host == 'windows':
        tools.append('idf-exe')
    cmd = [str(python), str(sandbox.idf_dir / 'tools' / 'idf_tools.py'),
           'install'] + tools
    reporter.info(f'平台：{sandbox.host}   目标目录：{sandbox.idf_tools_dir}')
    reporter.info('调用官方下载器 idf_tools.py install（会自动用国内镜像）')
    result = subprocess.run(cmd, env=env, cwd=str(sandbox.idf_dir),
                            creationflags=NO_WINDOW)
    return result.returncode == 0


def prepare_python_env(sandbox: Sandbox, reporter: Reporter) -> bool:
    """把这一套平台的 Python 环境准备好（主要给 Linux 用）

    Windows 侧是把现成的 site-packages 拷过来的，不用 pip；
    Linux 侧第一次得建个虚拟环境再把 ESP-IDF 的依赖装上（要联网）。
    """
    if sandbox.venv_python.is_file():
        reporter.info(f'Python 环境已就绪：{sandbox.venv_dir}')
        return True

    base = sandbox.python_exe
    if not base.is_file():
        base = Path(sys.executable)
        reporter.info(f'沙箱没带 Python，用系统的：{base}')
    else:
        reporter.info(f'用沙箱自带的 Python：{base}')

    reporter.info(f'创建虚拟环境：{sandbox.venv_dir}')
    result = subprocess.run([str(base), '-m', 'venv', '--system-site-packages',
                             str(sandbox.venv_dir)],
                            capture_output=True, text=True, encoding='utf-8',
                            errors='replace', creationflags=NO_WINDOW)
    if result.returncode != 0 or not sandbox.venv_python.is_file():
        reporter.error('创建虚拟环境失败')
        for line in ((result.stderr or '') + (result.stdout or '')).splitlines()[-6:]:
            reporter.hint(line)
        if sandbox.host != 'windows':
            reporter.hint('Debian/Ubuntu 上可能需要先装：sudo apt install python3-venv')
        return False

    req = sandbox.idf_dir / 'tools' / 'requirements' / 'requirements.core.txt'
    if not req.is_file():
        reporter.warn(f'找不到依赖清单 {req}，跳过装包')
        return True

    env = sandbox.build_env()
    pip = [str(sandbox.venv_python), '-m', 'pip']
    reporter.info('安装 ESP-IDF 的 Python 依赖（第一次要联网，几分钟）')
    for args in (['install', '--upgrade', 'pip'],
                 ['install', '-r', str(req)]):
        result = subprocess.run(pip + args, env=env, cwd=str(sandbox.idf_dir),
                                creationflags=NO_WINDOW)
        if result.returncode != 0:
            reporter.error(f'pip {" ".join(args)} 失败')
            reporter.hint('检查网络；国内网络慢的话可以先设 pip 镜像：')
            reporter.hint('  set PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple')
            return False
    return True


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def report_status(sandbox: Sandbox, reporter: Reporter) -> list[str]:
    reporter.banner('沙箱现状')
    missing = sandbox.check_essentials()
    reporter.line(f'  沙箱目录：{sandbox.root}')
    reporter.line(f'  当前系统：{sandbox.host}')
    reporter.line(f'  已装平台：{", ".join(sandbox.available_platforms()) or "（一个都没有）"}')
    if sandbox.root.is_dir():
        reporter.line(f'  体积    ：{human_size(dir_size(sandbox.root))}')
    reporter.line('')

    problem = sandbox.platform_problem()
    if problem:
        reporter.error('平台对不上：')
        for line in problem.splitlines():
            reporter.hint(line)
        reporter.line('')

    checks = [
        ('这一套工具链目录', sandbox.idf_tools_dir.is_dir(), str(sandbox.idf_tools_dir)),
        ('ESP-IDF 源码', sandbox.idf_py.is_file(),
         sandbox.idf_version or str(sandbox.idf_dir)),
    ]
    if sandbox.host == 'windows':
        checks.insert(0, ('便携 Python', sandbox.python_exe.is_file(), str(sandbox.python_exe)))
    checks.append(('Python 环境', sandbox.venv_python.is_file(), str(sandbox.venv_dir)))
    for name in ('xtensa-esp-elf', 'cmake', 'ninja', 'esp-rom-elfs'):
        path = sandbox.tool_path(name)
        checks.append((f'工具链 {name}', path is not None, path.name if path else '缺少'))
    for label, ok, detail in checks:
        mark = reporter.c('[OK]  ', 'green') if ok else reporter.c('[缺]  ', 'red')
        reporter.line(f'  {mark} {label:<18} {detail}')
    return missing


def main(argv: list[str] | None = None) -> int:
    init_console()
    args = parse_args(argv)
    sandbox = Sandbox(sandbox_dir=args.sandbox or None)
    reporter = open_log(sandbox, 'prepare')
    code = 0
    try:
        reporter.title('沙箱准备 / 修复')
        missing = report_status(sandbox, reporter)

        sources: list[Source] = []
        if args.source:
            root = Path(args.source)
            if looks_like_idf(root):
                sources.append(Source(f'指定的 {root}', root))
            elif (root / 'framework-espidf').is_dir():
                sources.append(Source(f'指定的 {root}', root / 'framework-espidf', root))
            elif (root / 'frameworks').is_dir() or (root / 'tools').is_dir():
                if (root / 'frameworks').is_dir():
                    for child in sorted((root / 'frameworks').iterdir(), reverse=True):
                        if looks_like_idf(child):
                            sources.append(Source(f'指定的 {root}', child,
                                                  root / 'tools', find_espressif_python(root)))
                            break
            else:
                reporter.warn(f'{root} 看起来不是 ESP-IDF 安装目录')
        sources += detect_sources()

        reporter.banner('本机找到的 ESP-IDF 来源')
        if sources:
            for source in sources:
                reporter.line('  - ' + repr(source))
        else:
            reporter.line('  （一个都没有）')

        if args.list_sources:
            return 0

        if not (args.repair or args.download):
            reporter.line('')
            if missing:
                reporter.warn('沙箱缺东西。加 --repair（从本机补齐）或 --download（从网上下）')
                code = 1
            else:
                reporter.ok('沙箱是完整的，不需要修')
            return code

        if args.download:
            reporter.banner('从官方渠道下载')
            if not sandbox.idf_py.is_file():
                if not download_idf(sandbox, reporter):
                    return 2
            if not prepare_python_env(sandbox, reporter):
                return 2
            if sandbox.missing_required_tools():
                if not download_tools(sandbox, reporter):
                    return 2
        elif sources and sandbox.host == 'windows':
            reporter.banner('从本机来源补齐')
            for source in sources:
                if not sandbox.python_exe.is_file():
                    install_python(sandbox, source, reporter, args.force)
                if sandbox.python_exe.is_file() and not sandbox.idf_py.is_file():
                    install_idf(sandbox, source, reporter, args.force)
                if sandbox.missing_required_tools():
                    install_tools(sandbox, source, reporter, args.force)
                if sandbox.python_exe.is_file():
                    install_python_deps(sandbox, source, reporter)
                if not sandbox.check_essentials():
                    break
        elif sources:
            reporter.banner(f'{sandbox.host} 上不搬 Windows 那套')
            reporter.hint('非 Windows 系统直接走官方下载：')
            reporter.hint(f'  python3 {sandbox.root / "start.py"} prepare --download')
            if not prepare_python_env(sandbox, reporter):
                return 2
            if sandbox.missing_required_tools():
                if not download_tools(sandbox, reporter):
                    return 2
        else:
            reporter.error('本机找不到现成的 ESP-IDF，只能用 --download 从网上下')
            code = 2

        # 收尾：重建虚拟环境 + 再体检一次
        reporter.banner('收尾')
        if sandbox.check_essentials():
            reporter.error('还差这些：')
            for item in sandbox.check_essentials():
                reporter.line(f'           {item}')
            code = 1
        else:
            from core import ensure_venv
            ensure_venv(sandbox, reporter)
            reporter.ok('沙箱齐了，可以点"一键编译烧录"了')
            code = 0
    except KeyboardInterrupt:
        reporter.warn('被 Ctrl+C 打断')
        code = 130
    except Exception as exc:
        reporter.error(f'出错了：{exc}')
        code = 1
    finally:
        reporter.line('')
        reporter.line(f'  日志：{reporter.log_file}')
        reporter.close()
        pause_if_needed(not args.no_pause, reporter)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
