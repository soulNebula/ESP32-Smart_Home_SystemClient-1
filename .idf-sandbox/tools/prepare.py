# -*- coding: utf-8 -*-
"""沙箱准备 / 修复

正常情况下用不到这个脚本：沙箱 (.idf-sandbox) 是随工程一起拷过来的，开箱即用。
只有在下面这些情况下才需要它：

* 沙箱只拷了一半，或者被杀毒软件删掉了几个文件
* 想在一台新的开发机上重新攒一套沙箱
* 想升级 / 换一个 ESP-IDF 版本

三种用法::

    python .idf-sandbox/tools/prepare.py                      # 只体检，看缺什么
    python .idf-sandbox/tools/prepare.py --repair             # 从本机已装的 ESP-IDF 补齐
    python .idf-sandbox/tools/prepare.py --download           # 缺什么就从网上下什么

``--repair`` 会自己去找本机的 ESP-IDF：

* 官方安装器装的（``C:\\Espressif`` / ``E:\\Espressif`` / ``%IDF_TOOLS_PATH%`` 等）
* PlatformIO 的包目录（``~/.platformio/packages``）
* 环境变量 ``IDF_PATH`` 指的目录

``--download`` 走官方渠道：ESP-IDF 源码走 GitHub release 压缩包，
工具链走 ``idf_tools.py install``（Espressif 官方下载器，走国内镜像）。
便携 Python 不在下载范围里 —— 在线配置直接用这台电脑的 Python 建虚拟环境。
"""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import shutil
import subprocess
import sys
import tarfile
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
                  dir_size, open_log, pause_if_needed, _python_version)

IDF_VERSION = '5.4.4'
IDF_GITHUB_ASSETS = 'dl.espressif.com/github_assets'
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
    parser.add_argument('--download', action='store_true',
                        help='在线配置：缺什么就从官方渠道下什么（推荐）')
    parser.add_argument('--check', action='store_true',
                        help='只做自检：每一项都真跑一遍，看能不能用')
    parser.add_argument('--prune', action='store_true',
                        help='清理可以重新生成/重新下载的东西，腾硬盘空间')
    parser.add_argument('--deep', action='store_true',
                        help='配合 --prune：连工具链和 ESP-IDF 源码也删（下次要重新下）')
    parser.add_argument('--yes', action='store_true',
                        help='配合 --prune：真的删（不加只预览）')
    parser.add_argument('--fetch', default='', metavar='平台',
                        help='给另一个平台备工具链（windows / linux），'
                             '这样整个文件夹拷到那台机器就能直接用')
    parser.add_argument('--repair', action='store_true', help='从本机已装的 ESP-IDF 补齐缺的东西')
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
    size = 0
    for candidate in IDF_ZIP_URLS:
        try:
            reporter.info(f'试探下载地址：{candidate}')
            with urllib.request.urlopen(candidate, timeout=20) as response:
                if response.status == 200:
                    url = candidate
                    size = int(response.headers.get('Content-Length') or 0)
                    break
        except Exception as exc:
            reporter.warn(f'连不上（{exc}）')
    if not url:
        reporter.error('两个下载地址都连不上，检查网络')
        return False

    archive = sandbox.root / 'download' / f'esp-idf-v{IDF_VERSION}.zip'
    archive.parent.mkdir(parents=True, exist_ok=True)
    how_big = human_size(size) if size else '约 1.9 GB'
    reporter.info(f'下载 ESP-IDF {IDF_VERSION} 源码包（{how_big}，耐心等；'
                  f'解压后只留 {human_size(400 * 1024 * 1024)} 左右）')
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

    # zip 里没有 version.txt（那是 git 检出的产物），补一个。
    # 没有它 idf.py 会给组件管理器一个空的 ESP_IDF_VERSION，直接抛异常。
    try:
        dest.mkdir(parents=True, exist_ok=True)
        (dest / 'version.txt').write_text(f'v{IDF_VERSION}\n', encoding='utf-8')
    except OSError:
        pass
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


def _run_streamed(cmd: list, env: dict, cwd: Path, reporter: Reporter,
                  tail: int = 12) -> bool:
    """跑一个子进程，把它的输出一行行转给 reporter

    为什么不直接 subprocess.run(继承 stdout)：官方下载器（idf_tools.py）会刷
    进度条、带颜色，直接在 GBK 控制台上吐字会 **UnicodeEncodeError 把自己搞死**，
    而且它的报错也会丢在控制台里看不见。这里统一抓过来自己解码，
    既能显示在 GUI 里，出错了也能把最后几行留下来给用户看。
    """
    try:
        proc = subprocess.Popen(
            cmd, env=env, cwd=str(cwd), stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
    except OSError as exc:
        reporter.error(f'启动子进程失败：{exc}')
        return False

    recent: list[str] = []
    assert proc.stdout is not None
    for raw in proc.stdout:
        text = raw.decode('utf-8', 'replace').rstrip('\r\n')
        if not text.strip():
            continue
        recent.append(text)
        if len(recent) > tail:
            recent.pop(0)
        reporter.raw(f'    {text[:110]}\n')
    code = proc.wait()
    if code != 0:
        reporter.error(f'子进程退出码 {code}，最后几行：')
        for line in recent[-tail:]:
            reporter.hint(line[:110])
    return code == 0


def download_tools(sandbox: Sandbox, reporter: Reporter) -> bool:
    """用官方下载器装这一套平台对应的工具链

    ``idf_tools.py`` 会按**当前操作系统**挑包，所以：

    * 在 Windows 上跑，装的是 ``idf_tools/``（*.exe）
    * 在 Linux 上跑，装的是 ``idf_tools-linux/``（ELF）

    自带国内镜像，速度还行（实测 2~5 MB/s）。
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
    reporter.info('调用官方下载器 idf_tools.py install（自动走国内镜像，约 400 MB）')
    return _run_streamed(cmd, env, sandbox.idf_dir, reporter)


PY_PROBE = ('import click, serial, cryptography, yaml, elftools, '
            'kconfiglib, esp_idf_monitor; print("ok")')


def python_env_ok(sandbox: Sandbox) -> bool:
    """现有 Python 环境真的能用吗（光看文件在不在不够）"""
    if not sandbox.venv_python.is_file():
        return False
    try:
        result = subprocess.run(
            [str(sandbox.venv_python), '-c', PY_PROBE],
            capture_output=True, text=True, encoding='utf-8', errors='replace',
            timeout=180, creationflags=NO_WINDOW)
        return result.returncode == 0 and 'ok' in (result.stdout or '')
    except Exception:
        return False


def prepare_python_env(sandbox: Sandbox, reporter: Reporter,
                       reinstall: bool = False) -> bool:
    """把这一套平台的 Python 环境准备好

    用 **ESP-IDF 官方的 install-python-env**，而不是自己 `pip install -r`：

    * 官方会带上 ``constraints.txt`` 的版本约束，装出来的组合是验证过的。
      自己装很容易出现"包装上了但 idf.py 跑不起来"（客户就撞上了这个）
    * 它默认走 ``dl.espressif.com`` 的 PyPI 镜像，国内不用翻墙也快

    官方那条路走不通，再退回自己建 venv + pip（带清华镜像兜底）。
    """
    if sandbox.venv_python.is_file() and not reinstall:
        if python_env_ok(sandbox):
            reporter.info(f'Python 环境已就绪：{sandbox.venv_dir}')
            return True
        reporter.warn('现有 Python 环境不完整，重装一次')
        reinstall = True

    base = sandbox.python_exe
    if not base.is_file():
        base = Path(sys.executable)
        reporter.info(f'沙箱没带 Python，用系统的：{base}  '
                      f'({_python_version(base)})')
    else:
        reporter.info(f'用沙箱自带的 Python：{base}')

    # ---- 首选：官方 install-python-env ----
    idf_tools = sandbox.idf_dir / 'tools' / 'idf_tools.py'
    if idf_tools.is_file():
        reporter.info('调用官方 install-python-env（带版本约束 + 国内 PyPI 镜像）')
        if reinstall:
            shutil.rmtree(sandbox.venv_dir, ignore_errors=True)
        sandbox.venv_dir.parent.mkdir(parents=True, exist_ok=True)
        env = sandbox.build_env()
        env['IDF_PYTHON_ENV_PATH'] = str(sandbox.venv_dir)
        env['IDF_PYTHON_CHECK_CONSTRAINTS'] = 'yes'
        cmd = [str(base), str(idf_tools), 'install-python-env', '--features', 'core']
        if reinstall:
            cmd.append('--reinstall')
        if _run_streamed(cmd, env, sandbox.idf_dir, reporter, tail=15):
            if python_env_ok(sandbox):
                return True
            reporter.warn('官方装完了但依赖还是导不进来，换备用方式再试')

    # ---- 备用：自己建 venv + pip ----
    reporter.info('改用备用方式：自己建虚拟环境 + pip 装依赖')
    shutil.rmtree(sandbox.venv_dir, ignore_errors=True)
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
        return python_env_ok(sandbox)

    env = sandbox.build_env()
    pip = [str(sandbox.venv_python), '-m', 'pip', 'install',
           '--disable-pip-version-check', '--no-color', '--progress-bar', 'off']
    reporter.info('安装 ESP-IDF 的 Python 依赖（第一次要联网，几分钟）')
    # 官方镜像 -> 清华 -> 默认源，总有一个能通
    mirrors = [
        ['-i', 'https://dl.espressif.com/pypi'],
        ['-i', 'https://pypi.tuna.tsinghua.edu.cn/simple'],
        [],
    ]
    for extra in mirrors:
        if extra:
            reporter.info(f'用镜像 {extra[1]}')
        result = subprocess.run(pip + extra + ['--upgrade', '-r', str(req)],
                                env=env, cwd=str(sandbox.idf_dir),
                                creationflags=NO_WINDOW)
        if result.returncode == 0 and python_env_ok(sandbox):
            return True
    reporter.error('装依赖失败')
    reporter.hint('检查网络；也可以自己指定镜像再试：')
    reporter.hint('  set PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple')
    return False


# ---------------------------------------------------------------------------
# 在线配置：联网把沙箱搭起来，再自检
# ---------------------------------------------------------------------------

def _step(reporter: Reporter, index: int, total: int, title: str, state: str) -> None:
    mark = {'ok': '[已有]', 'new': '[下载]', 'skip': '[跳过]', 'fail': '[失败]'}.get(state, '[····]')
    color = {'ok': 'green', 'new': 'cyan', 'skip': 'yellow', 'fail': 'red'}.get(state, 'white')
    reporter.line('')
    reporter.line(reporter.c(f'  {mark} [{index}/{total}] {title}', color))


def ensure_components(sandbox: Sandbox, reporter: Reporter) -> bool:
    """把工程的组件依赖（esp-sr 那些）拉到 managed_components/

    靠的是 ESP-IDF 官方的组件管理器，从 components.espressif.com 下载。
    这一步只动**被编译工程**目录下的 managed_components/，不碰系统。
    """
    project = sandbox.project
    marker = project / 'managed_components'
    manifest = project / 'main' / 'idf_component.yml'
    if marker.is_dir() and any(marker.iterdir()):
        reporter.info(f'组件依赖已就绪（{human_size(dir_size(marker))}）')
        return True
    if not manifest.is_file():
        reporter.info('这个工程没有 idf_component.yml，不需要下组件')
        return True
    if not sandbox.idf_py.is_file():
        reporter.warn('没有 ESP-IDF 源码，跳过组件下载')
        return False

    reporter.info('用组件管理器拉依赖（第一次要联网，约 230 MB）')
    env = sandbox.build_env(component_manager=True)   # 这一步必须开着组件管理器
    cmd = [str(sandbox.venv_python), str(sandbox.idf_py), 'reconfigure']
    if not _run_streamed(cmd, env, project, reporter, tail=20):
        reporter.error('组件下载失败')
        reporter.hint('检查网络；也可以手动跑一次看详细报错：')
        reporter.hint(f'  {sandbox.venv_python} {sandbox.idf_py} reconfigure')
        return False
    return marker.is_dir()


def self_check(sandbox: Sandbox, reporter: Reporter) -> bool:
    """配完自检 —— 每一项都真的跑一下，不只看文件在不在

    这是"下完了到底能不能用"的唯一答案。光看文件存在是不够的：
    有可能下载被截断、解压出错、或者 Python 依赖装了一半。
    """
    reporter.banner('自检')
    problems: list[str] = []

    # 1) 沙箱骨架
    if sandbox.idf_py.is_file():
        reporter.ok(f'ESP-IDF 源码        {sandbox.idf_version or "?"}')
    else:
        problems.append('缺 ESP-IDF 源码')
        reporter.error('ESP-IDF 源码        缺')

    # 2) Python 环境真的能 import（不是只看文件在不在）
    if python_env_ok(sandbox):
        reporter.ok(f'Python 环境         {_python_version(sandbox.venv_python)}')
    elif sandbox.venv_python.is_file():
        problems.append('Python 依赖装得不全')
        reporter.error('Python 依赖         导入失败')
        try:
            result = subprocess.run([str(sandbox.venv_python), '-c', PY_PROBE],
                                    capture_output=True, text=True, encoding='utf-8',
                                    errors='replace', timeout=120,
                                    creationflags=NO_WINDOW)
            tail = (result.stderr or '').strip().splitlines()
            for line in tail[-4:]:
                reporter.hint(line[:100])
        except Exception:
            pass
        reporter.hint('点「配置沙箱环境」会检测到并自动重装')
    else:
        problems.append('没有 Python 环境')
        reporter.error('Python 环境         缺')
        reporter.hint('点「配置沙箱环境」会装好')

    # 3) 交叉编译器真的能跑
    gcc = None
    tool_dir = sandbox.idf_tools_dir / 'tools' / 'xtensa-esp-elf'
    if tool_dir.is_dir():
        for path in tool_dir.rglob('xtensa-esp32s3-elf-gcc*'):
            if path.is_file():
                gcc = path
                break
    if gcc is not None:
        env = sandbox.build_env()
        try:
            result = subprocess.run([str(gcc), '--version'], env=env,
                                    capture_output=True, text=True,
                                    encoding='utf-8', errors='replace',
                                    timeout=60, creationflags=NO_WINDOW)
            if result.returncode == 0:
                first = (result.stdout or '').splitlines()[0][:58]
                reporter.ok(f'交叉编译器          {first}')
            else:
                problems.append('交叉编译器跑不起来')
                reporter.error('交叉编译器          执行失败')
        except Exception as exc:
            problems.append(f'交叉编译器跑不起来：{exc}')
            reporter.error(f'交叉编译器          {exc}')
    else:
        problems.append('缺交叉编译器')
        reporter.error('交叉编译器          缺')

    # 4) 端到端：让 idf.py 自己说版本（这一条过了基本就稳了）
    if sandbox.idf_py.is_file() and sandbox.venv_python.is_file():
        try:
            env = sandbox.build_env()
            result = subprocess.run(
                [str(sandbox.venv_python), str(sandbox.idf_py), '--version'],
                env=env, cwd=str(sandbox.project), capture_output=True,
                text=True, encoding='utf-8', errors='replace', timeout=180,
                creationflags=NO_WINDOW)
            text = ((result.stdout or '') + (result.stderr or '')).strip()
            if result.returncode == 0 and 'ESP-IDF' in text:
                reporter.ok(f'idf.py 端到端       {text.splitlines()[0][:58]}')
            else:
                problems.append('idf.py 跑不起来')
                reporter.error(f'idf.py 端到端       失败（退出码 {result.returncode}）')
                # 把 idf.py 真正吐出来的东西原样贴出来 ——
                # 光说"失败"用户没法反馈，我们也无从下手（客户那次就卡在这）
                shown = [ln for ln in text.splitlines() if ln.strip()]
                if shown:
                    reporter.line('          ↓ idf.py 的原始输出（反馈问题时把这段一起发来）')
                    for line in shown[-12:]:
                        reporter.hint(line[:110])
                else:
                    reporter.hint('（idf.py 一个字都没输出，通常是被杀毒软件拦了）')
                reporter.line('')
                reporter.hint('常见原因和对应做法：')
                reporter.hint('  · 依赖没装全 → 再点一次「配置沙箱环境」'
                              '（它会检测到并重装）')
                reporter.hint('  · 杀毒软件拦截 python.exe → 把 .idf-sandbox 加进白名单')
                reporter.hint(f'  · 想自己看详细报错，手动跑：')
                reporter.hint(f'      {sandbox.venv_python} {sandbox.idf_py} --version')
        except Exception as exc:
            problems.append(f'idf.py 跑不起来：{exc}')
            reporter.error(f'idf.py 端到端       {exc}')

    # 5) 组件依赖
    comps = sandbox.project / 'managed_components'
    if comps.is_dir() and any(comps.iterdir()):
        reporter.ok(f'组件依赖            {human_size(dir_size(comps))}')
    elif (sandbox.project / 'main' / 'idf_component.yml').is_file():
        reporter.warn('组件依赖            还没有（第一次编译时会自动下）')
    else:
        reporter.ok('组件依赖            这个工程不需要')

    reporter.line('')
    if problems:
        reporter.error('自检没过，还差：')
        for item in problems:
            reporter.line(f'           {item}')
        reporter.hint('再跑一次会自动续传：python .idf-sandbox/start.py prepare --download')
        return False
    reporter.ok('自检全部通过 —— 可以点"一键编译烧录"了')
    return True


def online_setup(sandbox: Sandbox, reporter: Reporter) -> bool:
    """联网把沙箱配齐：缺什么下什么，已有的跳过，最后自检

    所有东西都落在 `.idf-sandbox/` 里，不往系统目录写一个字节：
    不装 C 编译器、不动 PATH、不碰注册表、不装全局 Python 包。
    下载的压缩包放在 `download/`，解压完立刻删掉。
    """
    reporter.banner('在线配置沙箱环境')
    reporter.line(f'  沙箱目录：{sandbox.root}')
    reporter.line(f'  当前系统：{sandbox.host}')
    reporter.line(f'  当前解释器：{sys.executable}  ({_python_version(Path(sys.executable))})')
    if sandbox.host != 'windows':
        reporter.line('  （Linux 上用系统自带的 python3，不额外下载 Python）')
    reporter.line('')
    reporter.line('  全部下载到沙箱目录内，不污染这台电脑。')
    reporter.line('  中途断了不要紧，再跑一次会接着下。')

    total = 4
    cache = sandbox.root / 'download'
    try:
        cache.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass

    # 1) ESP-IDF 源码（工具链要靠它的 tools.json 才知道从哪下）
    if sandbox.idf_py.is_file():
        _step(reporter, 1, total, 'ESP-IDF 源码', 'ok')
        reporter.info(f'已有 {sandbox.idf_version}')
    else:
        _step(reporter, 1, total, 'ESP-IDF 源码', 'new')
        if not download_idf(sandbox, reporter):
            return False

    # 2) Python 环境（建 venv + 装 ESP-IDF 的依赖）
    #    注意：光看 venv 在不在不够 —— 客户那次就是"文件都在，但依赖导不进来"，
    #    于是 idf.py 跑不起来。这里真的 import 一遍，坏了就重装。
    if python_env_ok(sandbox):
        _step(reporter, 2, total, 'Python 环境', 'ok')
        reporter.info(f'{sandbox.venv_dir}  ({_python_version(sandbox.venv_python)})')
    else:
        broken = sandbox.venv_python.is_file()
        _step(reporter, 2, total, 'Python 环境', 'new')
        if broken:
            reporter.warn('现有的环境不完整（依赖导不进来），重装一遍')
        if not prepare_python_env(sandbox, reporter, reinstall=broken):
            return False

    # 3) 交叉工具链
    missing = sandbox.missing_required_tools()
    if missing:
        _step(reporter, 3, total, f'交叉工具链（缺 {", ".join(missing)}）', 'new')
        if not download_tools(sandbox, reporter):
            return False
    else:
        _step(reporter, 3, total, '交叉工具链', 'ok')
        reporter.info(f'{human_size(dir_size(sandbox.idf_tools_dir))}')

    # 4) 工程组件依赖
    _step(reporter, 4, total, '工程组件依赖', 'new')
    ensure_components(sandbox, reporter)

    # 收尾：把下载缓存删掉，别占地方
    cleanup_downloads(sandbox, reporter)

    return self_check(sandbox, reporter)


def cleanup_downloads(sandbox: Sandbox, reporter: Reporter) -> None:
    """下载缓存用完就删 —— 它们都能重新下，留着纯占地方"""
    cache = sandbox.root / 'download'
    if not cache.is_dir():
        return
    try:
        freed = dir_size(cache)
        shutil.rmtree(cache, ignore_errors=True)
        if freed:
            reporter.info(f'清掉下载缓存，释放 {human_size(freed)}')
    except OSError:
        pass


def prune(sandbox: Sandbox, reporter: Reporter, deep: bool = False,
          really: bool = False) -> int:
    """删掉"没了也能再弄回来"的东西，给硬盘腾地方

    默认删这些（都是缓存/产物，不影响沙箱本身）：

    * ``build/``                    编译产物，下次编译自动重建
    * ``.idf-sandbox/download/``    下载缓存，解压完就没用了
    * ``.idf-sandbox/logs/`` ``tmp/``
    * 所有 ``__pycache__``
    * **另一个平台**的工具链         Windows 上留着 Linux 那套没用，反之亦然

    ``deep=True`` 时连**当前平台**的工具链也删掉 —— 下次点「配置沙箱环境」
    能重新下回来（约 400 MB，几分钟）。这一条只在你确定暂时不需要编译时用。
    """
    other = 'linux' if sandbox.host == 'windows' else 'windows'
    plan: list[tuple[str, Path]] = [
        ('编译产物 build/', sandbox.project / 'build'),
        ('下载缓存 download/', sandbox.root / 'download'),
        ('日志 logs/', sandbox.root / 'logs'),
        ('临时 tmp/', sandbox.root / 'tmp'),
        (f'另一个平台的工具链 idf_tools-{other}/（当前系统是 {sandbox.host}）',
         sandbox.root / f'idf_tools-{other}'),
        (f'另一个平台的虚拟环境 penv-{other}/', sandbox.root / f'penv-{other}'),
        (f'另一个平台的 Python python-{other}/', sandbox.root / f'python-{other}'),
    ]
    if deep:
        plan.append((f'当前平台的工具链（{sandbox.host}，约 1.5 GB）',
                     sandbox.idf_tools_dir))
        plan.append(('ESP-IDF 源码（约 355 MB）', sandbox.idf_dir))
        plan.append(('工程组件依赖 managed_components/', sandbox.project / 'managed_components'))

    reporter.banner('清理可以重新生成的东西')
    if not really:
        reporter.warn('这是预览（加 --yes 才真删）：')
    freed = 0
    for label, path in plan:
        if not path.exists():
            continue
        size = dir_size(path) if path.is_dir() else path.stat().st_size
        if not really:
            reporter.line(f'  · {label:<44} {human_size(size)}')
            freed += size
            continue
        try:
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
            else:
                path.unlink()
            reporter.ok(f'  删掉 {label}（{human_size(size)}）')
            freed += size
        except OSError as exc:
            reporter.warn(f'  删不掉 {label}：{exc}')

    # __pycache__ 到处都是，单独扫一遍
    cache_total = 0
    for path in list(sandbox.root.rglob('__pycache__')) + \
            list(sandbox.project.rglob('__pycache__')):
        if '__pycache__' not in path.parts:
            continue
        try:
            size = dir_size(path)
            if really:
                shutil.rmtree(path, ignore_errors=True)
            cache_total += size
        except OSError:
            pass
    if cache_total:
        freed += cache_total
        verb = '删掉' if really else '可删'
        reporter.line(f'  · {verb} __pycache__（{human_size(cache_total)}）')

    reporter.line('')
    if freed:
        verb = '已释放' if really else '可以释放'
        reporter.ok(f'{verb} {human_size(freed)}')
    else:
        reporter.ok('已经很干净了，没东西可删')
    if not really and freed:
        reporter.hint('确认要删就加 --yes：')
        reporter.hint(f'  python {sandbox.root / "start.py"} prepare --prune --yes')
    return 0


# ---------------------------------------------------------------------------
# 给另一个平台备货（在 Windows 上把 Linux 那套下好，反之亦然）
# ---------------------------------------------------------------------------

def _mirror(url: str) -> str:
    """github.com/... -> Espressif 镜像（国内直连 github 经常超时）"""
    prefix = 'https://github.com/'
    if url.startswith(prefix):
        return f'https://{IDF_GITHUB_ASSETS}/' + url[len(prefix):]
    return url


def _extract_tool(archive: Path, target: Path, strip_top: int,
                  reporter: Reporter) -> None:
    """解压工具链压缩包

    * ``strip_top=1`` 丢掉最外层那一个目录
    * **符号链接解引用成真文件** —— Windows 上造不出符号链接，
      而 tar 里那几个 ``liblto_plugin.so`` 之类是真要用的。
      （第一版就是在这儿静默丢过东西：``os.path.join`` 在 Windows 上拼出
      反斜杠，跟 tar 成员名对不上，链接全被跳过。tar 内部路径必须用 posixpath。）
    """
    name = archive.name.lower()
    target.mkdir(parents=True, exist_ok=True)

    if name.endswith('.zip'):
        with zipfile.ZipFile(archive) as zf:
            for member in zf.infolist():
                parts = Path(member.filename).parts[strip_top:]
                if not parts:
                    continue
                out = target.joinpath(*parts)
                if member.is_dir():
                    out.mkdir(parents=True, exist_ok=True)
                else:
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with zf.open(member) as src, open(out, 'wb') as dst:
                        shutil.copyfileobj(src, dst, 1024 * 512)
        return

    with tarfile.open(archive) as tf:
        members = tf.getmembers()
        by_name = {m.name: m for m in members}
        for member in members:
            parts = Path(member.name).parts[strip_top:]
            if not parts:
                continue
            out = target.joinpath(*parts)
            if member.isdir():
                out.mkdir(parents=True, exist_ok=True)
                continue
            source = _resolve_link(member, by_name)
            if source is None or not source.isfile():
                continue
            out.parent.mkdir(parents=True, exist_ok=True)
            handle = tf.extractfile(source)
            if handle is None:
                continue
            with handle, open(out, 'wb') as dst:
                shutil.copyfileobj(handle, dst, 1024 * 512)


def _resolve_link(member, by_name):
    """把链接成员解析成真正的文件成员（链接可能指向另一个链接）"""
    for _ in range(8):
        if member.issym():
            base = posixpath.dirname(member.name)
            target = posixpath.normpath(posixpath.join(base, member.linkname))
        elif member.islnk():
            target = posixpath.normpath(member.linkname)
        else:
            return member
        nxt = by_name.get(target)
        if nxt is None:
            return None
        member = nxt
    return None


def fetch_platform(sandbox: Sandbox, reporter: Reporter, want: str) -> bool:
    """把**另一个平台**的工具链下好，放在 ``idf_tools-<平台>/``

    官方下载器（idf_tools.py）只会下"当前系统"的包，所以这里自己读
    ``tools.json`` 里的地址下 —— 走 Espressif 镜像。

    用途：在 Windows 上把 Linux 那套备齐，整个文件夹拷到 Linux 机器就能直接用，
    不用在那台机器上再下一次。
    """
    if want == sandbox.host:
        reporter.info(f'当前就是 {want}，不需要另外备货')
        return True
    if not sandbox.tools_json.is_file():
        reporter.error('还没有 ESP-IDF 源码（tools.json 在里面），先跑 --download')
        return False

    key = {'windows': 'win64', 'linux': 'linux-amd64', 'macos': 'macos'}.get(want)
    if not key:
        reporter.error(f'不认识的平台：{want}')
        return False

    try:
        data = json.loads(sandbox.tools_json.read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        reporter.error(f'读不了 tools.json：{exc}')
        return False

    root = sandbox.root / f'idf_tools-{want}' / 'tools'
    cache = sandbox.root / 'download'
    cache.mkdir(parents=True, exist_ok=True)

    wanted = ['xtensa-esp-elf', 'cmake', 'ninja', 'esp-rom-elfs']
    if want == 'windows':
        wanted.append('idf-exe')

    reporter.banner(f'给 {want} 备工具链')
    reporter.line(f'  目标目录：{root}')
    reporter.line(f'  下载缓存：{cache}（下完就删）')
    total_bytes = 0

    for tool in data.get('tools', []):
        name = tool.get('name')
        if name not in wanted:
            continue
        if not tool.get('versions'):
            continue
        version = tool['versions'][0]
        info = version.get(key) or version.get('any')
        if not info or not info.get('url'):
            reporter.warn(f'{name} 没有 {want} 版的包，跳过')
            continue
        strip = tool.get('strip_container_dirs', 0)
        target = root / name / version['name']
        if target.is_dir() and any(target.iterdir()):
            reporter.info(f'{name} 已经有了，跳过')
            continue

        url = _mirror(info['url'])
        archive = cache / os.path.basename(info['url'].split('?')[0])
        reporter.info(f'{name} {version["name"]}  {url.split("/")[-1]}')
        try:
            if not archive.is_file():
                _download_with_progress(url, archive, reporter)
            shutil.rmtree(target, ignore_errors=True)
            _extract_tool(archive, target, strip, reporter)
            total_bytes += dir_size(target)
        except Exception as exc:
            reporter.error(f'{name} 失败：{exc}')
            return False
        finally:
            if archive.is_file():
                try:
                    archive.unlink()
                except OSError:
                    pass

    # 光有文件不够 —— 得确认里面真的是那个平台的可执行文件
    gcc = None
    for path in root.rglob('xtensa-esp32s3-elf-gcc*'):
        if path.is_file():
            gcc = path
            break
    if gcc is None:
        reporter.error('下完了但找不到编译器，可能地址变了')
        return False
    with open(gcc, 'rb') as fh:
        magic = fh.read(4)
    kind = 'windows' if gcc.suffix.lower() == '.exe' else 'linux'
    if kind != want:
        reporter.error(f'下下来的居然是 {kind} 版，平台不对')
        return False
    reporter.ok(f'{want} 工具链就绪：{human_size(total_bytes)}，'
                f'编译器格式 {kind}（{"PE/.exe" if kind == "windows" else "ELF"}）')
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

        # --download 是"在线配置"，直接走它自己的流程，不用先满世界找本机 IDF
        if args.download:
            return 0 if online_setup(sandbox, reporter) else 2
        if args.check:
            return 0 if self_check(sandbox, reporter) else 1
        if args.prune:
            return prune(sandbox, reporter, deep=args.deep, really=args.yes)
        if args.fetch:
            return 0 if fetch_platform(sandbox, reporter, args.fetch.lower()) else 2

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

        if args.repair and sources and sandbox.host == 'windows':
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
