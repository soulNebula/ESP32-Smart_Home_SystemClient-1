# -*- coding: utf-8 -*-
"""ESP32-S3 智能家居 —— 便携沙箱核心

这个文件是整套"绿色沙箱"的地基，负责四件事：

1. 找到工程目录和沙箱目录（默认 ``工程根/.idf-sandbox``）
2. 校验沙箱里的三样东西：便携 Python、ESP-IDF 源码、交叉工具链
3. 拼出一套**干净**的环境变量（客户机上装没装东西都不影响）
4. 提供统一的输出、日志、串口扫描等小工具给其它脚本用

沙箱目录结构（可以整体拷贝到任何电脑，不用重装）：:

    .idf-sandbox/
    ├── python/      便携 Python 3.11.2（依赖装在它自己的 site-packages 里）
    ├── penv/        虚拟环境（只放一个壳，依赖复用 python/ 里的，换电脑能自动重建）
    ├── idf/         ESP-IDF 源码（IDF_PATH）
    ├── idf_tools/   工具链家目录（IDF_TOOLS_PATH）
    │   └── tools/{xtensa-esp-elf,cmake,ninja,esp-rom-elfs,idf-exe}/<版本>/
    └── sandbox.json 沙箱清单（记录版本，方便排查）
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

# 全部脚本都支持 Python 3.9 ~ 3.14。
# 能这么写是因为每个文件都带了 `from __future__ import annotations`，
# 注解里的 `Path | None`、`list[str]` 这类写法不会被求值，3.9 上也照样跑。
# 这里兜一道底：更老的解释器直接给一句人话，而不是抛一堆看不懂的 traceback。
if sys.version_info < (3, 9):
    raise SystemExit('[X] 需要 Python 3.9 或更高版本，当前是 ' + sys.version.split()[0]
                     + '\n    请改用沙箱自带的解释器：.idf-sandbox\\python\\python.exe')

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

SANDBOX_DIRNAME = '.idf-sandbox'

# 编译固件必须有这几样，少一个都编不过
REQUIRED_TOOLS = ('xtensa-esp-elf', 'cmake', 'ninja', 'esp-rom-elfs', 'idf-exe')

# 有更好、没有也能编的（调试器、烧录辅助等）
OPTIONAL_TOOLS = ('ccache', 'riscv32-esp-elf', 'esp32ulp-elf', 'openocd-esp32',
                  'xtensa-esp-elf-gdb', 'riscv32-esp-elf-gdb', 'dfu-util')

# 默认目标芯片（改这里没用，真正的目标在 sdkconfig 的 CONFIG_IDF_TARGET）
DEFAULT_TARGET = 'esp32s3'

# app 分区读不到时的兜底大小（本工程的 ota_0 是 4 MiB）
DEFAULT_APP_PARTITION_SIZE = 0x400000

# 常见 USB 转串口芯片的 VID，用来猜哪个口是开发板
KNOWN_USB_VIDS = {
    0x303A: 'Espressif 原生 USB',
    0x10C4: 'Silicon Labs CP210x',
    0x1A86: 'WCH CH34x/CH91xx',
    0x0403: 'FTDI',
    0x067B: 'Prolific PL2303',
    0x2341: 'Arduino',
}


# ---------------------------------------------------------------------------
# 小工具
# ---------------------------------------------------------------------------

def enable_ansi_colors() -> bool:
    """让 Windows 控制台认识 ANSI 颜色码（Win10 以上都行）"""
    if os.name != 'nt':
        return True
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))
    except Exception:
        return False


def init_console() -> None:
    """统一控制台编码

    接到真正的控制台时，Python 走的是 Windows 的 Unicode 接口，中文本来就正常；
    一旦输出被重定向（管道/文件），就强制用 UTF-8，免得跟着系统的 GBK 跑偏。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            if hasattr(stream, 'isatty') and stream.isatty():
                stream.reconfigure(errors='replace')
            else:
                stream.reconfigure(encoding='utf-8', errors='replace')
        except Exception:
            pass


def human_size(num_bytes: int) -> str:
    for unit in ('B', 'KB', 'MB', 'GB'):
        if num_bytes < 1024 or unit == 'GB':
            return f'{num_bytes:.0f} {unit}' if unit == 'B' else f'{num_bytes:.1f} {unit}'
        num_bytes /= 1024.0
    return f'{num_bytes:.1f} GB'


def dir_size(path: Path) -> int:
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total


# 图形界面（gui.py）是用 pythonw.exe 跑的，自己没有控制台。
# 这种情况下再去启动 gcc/cmake/ninja 这些控制台程序，Windows 会给每个子进程
# 弹一个黑框出来。加上 CREATE_NO_WINDOW 就都老实了。
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0x08000000) if os.name == 'nt' else 0


def _tidy_tmp(tmp_dir: Path, max_age_hours: float = 24.0) -> None:
    """顺手删掉一天前的临时文件，别让沙箱里的 tmp 越攒越多"""
    cutoff = time.time() - max_age_hours * 3600
    try:
        entries = list(os.scandir(tmp_dir))
    except OSError:
        return
    for entry in entries:
        try:
            if entry.stat().st_mtime > cutoff:
                continue
            if entry.is_dir(follow_symlinks=False):
                shutil.rmtree(entry.path, ignore_errors=True)
            else:
                os.unlink(entry.path)
        except OSError:
            pass


def is_ascii(text: str) -> bool:
    try:
        text.encode('ascii')
        return True
    except UnicodeEncodeError:
        return False


def host_platform() -> str:
    """当前操作系统：windows / linux / macos"""
    if sys.platform.startswith('win'):
        return 'windows'
    if sys.platform.startswith('linux'):
        return 'linux'
    if sys.platform == 'darwin':
        return 'macos'
    return sys.platform


def open_path(path) -> None:
    """用系统默认程序打开一个文件/文件夹

    Windows 是 os.startfile，Linux 是 xdg-open，macOS 是 open。
    之前只写了 os.startfile，Linux 上一按就报
    "module 'os' has no attribute 'startfile'"。
    """
    target = str(path)
    if os.name == 'nt':
        os.startfile(target)                      # type: ignore[attr-defined]
        return
    opener = 'open' if sys.platform == 'darwin' else 'xdg-open'
    subprocess.Popen([opener, target],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def parse_size(text: str) -> int:
    """把分区表里的 '0x400000' / '4M' / '1024K' 变成字节数"""
    text = (text or '').strip().upper()
    if not text:
        return 0
    mult = 1
    if text.endswith('K'):
        mult, text = 1024, text[:-1]
    elif text.endswith('M'):
        mult, text = 1024 * 1024, text[:-1]
    try:
        value = int(text, 16) if text.startswith('0X') else int(text)
    except ValueError:
        return 0
    return value * mult


def _app_size_from_csv(path: Path) -> int:
    """从分区表 CSV 里挑一个 app 分区的大小"""
    try:
        raw = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return 0
    ota_app = 0
    plain_app = 0
    for line in raw.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = [p.strip() for p in line.split(',')]
        if len(parts) < 5 or parts[1].lower() != 'app':
            continue
        size = parse_size(parts[4])
        if not size:
            continue
        if parts[2].lower().startswith('ota'):
            ota_app = ota_app or size
        else:
            plain_app = plain_app or size
    return ota_app or plain_app


# ---------------------------------------------------------------------------
# 输出与日志
# ---------------------------------------------------------------------------

class Reporter:
    """一边打印到屏幕、一边写日志"""

    COLORS = {
        'reset': '\x1b[0m', 'bold': '\x1b[1m', 'dim': '\x1b[2m',
        'red': '\x1b[31m', 'green': '\x1b[32m', 'yellow': '\x1b[33m',
        'blue': '\x1b[34m', 'cyan': '\x1b[36m', 'gray': '\x1b[90m',
    }

    def __init__(self, log_file: Path | None = None, color: bool | None = None):
        self.log_file = Path(log_file) if log_file else None
        self._fh = None
        if color is None:
            color = sys.stdout.isatty() if hasattr(sys.stdout, 'isatty') else False
            color = color and enable_ansi_colors()
        self.color = bool(color)
        if self.log_file:
            try:
                self.log_file.parent.mkdir(parents=True, exist_ok=True)
                self._fh = open(self.log_file, 'a', encoding='utf-8', errors='replace')
            except OSError:
                # 日志写不了不该让整个程序挂掉（比如工程目录是只读的）
                self._fh = None
                self.log_file = None

    # -- 基础 --

    def c(self, text: str, *styles: str) -> str:
        if not self.color or not styles:
            return text
        prefix = ''.join(self.COLORS.get(s, '') for s in styles)
        return f'{prefix}{text}{self.COLORS["reset"]}'

    def line(self, text: str = '') -> None:
        print(text)
        self._log(text)

    def raw(self, text: str) -> None:
        """子进程输出的原文，直接透传"""
        sys.stdout.write(text)
        sys.stdout.flush()
        self._log(text, newline=False)

    def raw_replace(self, text: str) -> None:
        """进度条那种用 \\r 原地刷新的输出：屏幕上覆盖同一行，日志里记成一行"""
        erase = '\x1b[K' if self.color else ''
        sys.stdout.write('\r' + text + erase)
        sys.stdout.flush()
        self._log(text)

    def _log(self, text: str, newline: bool = True) -> None:
        if not self._fh:
            return
        try:
            self._fh.write(text + ('\n' if newline else ''))
            self._fh.flush()
        except Exception:
            pass

    def close(self) -> None:
        if self._fh:
            try:
                self._fh.close()
            except Exception:
                pass
            self._fh = None

    # -- 语义化 --

    def title(self, text: str) -> None:
        bar = '=' * 64
        self.line(self.c(bar, 'cyan'))
        self.line(self.c('  ' + text, 'cyan', 'bold'))
        self.line(self.c(bar, 'cyan'))

    def banner(self, text: str) -> None:
        self.line('')
        self.line(self.c(f'--- {text} ' + '-' * max(0, 56 - len(text)), 'blue'))

    def ok(self, text: str) -> None:
        self.line(self.c('  [OK]   ', 'green') + text)

    def info(self, text: str) -> None:
        self.line(self.c('  [..]   ', 'gray') + text)

    def warn(self, text: str) -> None:
        self.line(self.c('  [警告] ', 'yellow') + text)

    def error(self, text: str) -> None:
        self.line(self.c('  [失败] ', 'red') + text)

    def hint(self, text: str) -> None:
        self.line(self.c('         ' + text, 'dim'))

    def step(self, index: int, total: int, text: str) -> None:
        tag = f'[{index}/{total}]'
        self.line('')
        self.line(self.c(f'{tag} {text}', 'bold'))


# ---------------------------------------------------------------------------
# 沙箱
# ---------------------------------------------------------------------------

class SandboxError(RuntimeError):
    """沙箱本身有问题（缺文件、路径不对……）"""


class Sandbox:
    """定位并描述一个便携沙箱

    这里有两个"工程"概念，别搞混：

    * ``root``    —— 沙箱自己的位置，就是 ``.idf-sandbox/``。工具链、Python、
                      脚本全在里面。从 ``core.py`` 的位置推出来，不依赖工作目录。
    * ``home``    —— 沙箱旁边那个工程（``root`` 的上级目录）。
    * ``project`` —— **要构建哪个工程**。默认等于 ``home``，也可以指向别的工程，
                      比如 ``Sandbox(project=r'D:\\我的另一个工程')``，
                      这时工具链还是用沙箱里的，源码/配置用对方的。
    """

    def __init__(self, project: str | os.PathLike | None = None,
                 sandbox_dir: str | os.PathLike | None = None):
        # 脚本自己住在 .idf-sandbox\tools\ 里，所以沙箱在哪儿是推出来的
        if sandbox_dir:
            self.root = Path(sandbox_dir).resolve()
        elif os.environ.get('IDF_SANDBOX_DIR'):
            self.root = Path(os.environ['IDF_SANDBOX_DIR']).resolve()
        else:
            self.root = sandbox_root()
        # 沙箱的上级目录，就是"这个沙箱服务的工程"
        self.home = self.root.parent
        self.project = Path(project).resolve() if project else self.home

    @property
    def is_foreign(self) -> bool:
        """构建的不是沙箱自己那个工程"""
        try:
            return self.project != self.home
        except OSError:
            return True

    # -- 目录 --
    #
    # 沙箱是**分平台**的：Windows 的工具链是 .exe，Linux 的是 ELF，两边不能混用。
    # 所以目录按平台取名：Windows 用 idf_tools/ + penv/，Linux 用
    # idf_tools-linux/ + penv-linux/。一个沙箱可以同时装两套（互不干扰），
    # 也可以只装一套 —— 用哪套由当前操作系统决定。

    def plat_dir(self, base: str) -> Path:
        """按平台取子目录：Windows 用原名，别的系统用 <名字>-<平台>"""
        if self.host == 'windows':
            return self.root / base
        return self.root / f'{base}-{self.host}'

    @property
    def host(self) -> str:
        """当前跑在什么系统上"""
        return host_platform()

    @property
    def python_dir(self) -> Path:
        """便携 Python 的目录（只有 Windows 版会打包，Linux 用系统自带的）"""
        if self.host == 'windows':
            return self.root / 'python'
        return self.root / f'python-{self.host}'

    @property
    def python_exe(self) -> Path:
        """沙箱自带的解释器

        Linux/macOS 上不一定打包 Python —— 返回的是一个"该在这儿"的路径，
        调用方自己用 ``is_file()`` 判断，不存在就退回系统 python3。
        """
        if self.host == 'windows':
            return self.python_dir / 'python.exe'
        return self.python_dir / 'bin' / 'python3'

    @property
    def venv_dir(self) -> Path:
        return self.plat_dir('penv')

    @property
    def venv_python(self) -> Path:
        if self.host == 'windows':
            return self.venv_dir / 'Scripts' / 'python.exe'
        return self.venv_dir / 'bin' / 'python'

    @property
    def idf_dir(self) -> Path:
        """ESP-IDF 源码：跟平台无关，两边共用同一份"""
        return self.root / 'idf'

    @property
    def idf_py(self) -> Path:
        return self.idf_dir / 'tools' / 'idf.py'

    @property
    def idf_tools_dir(self) -> Path:
        return self.plat_dir('idf_tools')

    @property
    def tools_json(self) -> Path:
        return self.idf_dir / 'tools' / 'tools.json'

    @property
    def drivers_dir(self) -> Path:
        return self.root / 'drivers'

    @property
    def state_file(self) -> Path:
        base = (os.environ.get('LOCALAPPDATA') or os.environ.get('XDG_STATE_HOME')
                or os.environ.get('TEMP') or os.path.expanduser('~'))
        return Path(base) / 'esp32-smarthome' / 'state.json'

    @property
    def log_dir(self) -> Path:
        """日志统一写在沙箱里，别弄脏工程目录"""
        return self.root / 'logs'

    # -- 平台是否对得上 --

    def tool_platform(self) -> str:
        """看工具链里装的是哪个平台的东西（.exe = Windows，否则 = Linux/类 Unix）"""
        tool_dir = self.idf_tools_dir / 'tools'
        if not tool_dir.is_dir():
            return ''
        for path in tool_dir.rglob('xtensa-esp32s3-elf-gcc*'):
            if path.is_file():
                return 'windows' if path.suffix.lower() == '.exe' else 'linux'
        return ''

    def platform_problem(self) -> str:
        """平台对不上就说清楚，对得上返回空串

        这是"在 Linux 上点 Windows 沙箱"这种情况的唯一提示来源 ——
        不然用户看到的是一连串 Exec format error，完全不知道发生了什么。
        """
        found = self.tool_platform()
        if not found:
            return ''                     # 还没装工具链，交给别的检查去报
        if found == self.host:
            return ''
        return (f'沙箱里的工具链是 **{found}** 版的，当前系统是 **{self.host}**。\n'
                f'两边的可执行文件格式不一样（.exe vs ELF），没法互相运行。')

    def available_platforms(self) -> list:
        """这个沙箱里已经装好了哪些平台的工具链"""
        found = []
        for name, plat in (('idf_tools', 'windows'),
                           ('idf_tools-linux', 'linux'),
                           ('idf_tools-macos', 'macos')):
            if (self.root / name / 'tools').is_dir():
                found.append(plat)
        return found

    # -- 版本信息 --

    @property
    def idf_version(self) -> str:
        """完整版本号，例如 5.4.4"""
        try:
            raw = (self.idf_dir / 'version.txt').read_text(encoding='utf-8', errors='replace').strip()
            return raw.lstrip('v')
        except OSError:
            return ''

    @property
    def idf_version_short(self) -> str:
        """主次版本号，例如 5.4（ESP_IDF_VERSION 要的是这个）"""
        m = re.match(r'(\d+)\.(\d+)', self.idf_version)
        return f'{m.group(1)}.{m.group(2)}' if m else self.idf_version

    @property
    def chip_target(self) -> str:
        """从 sdkconfig 里读目标芯片"""
        for name in ('sdkconfig', 'sdkconfig.defaults'):
            path = self.project / name
            try:
                text = path.read_text(encoding='utf-8', errors='replace')
            except OSError:
                continue
            m = re.search(r'^CONFIG_IDF_TARGET="([^"]+)"', text, re.M)
            if m:
                return m.group(1)
        return DEFAULT_TARGET

    @property
    def managed_components(self) -> Path | None:
        path = self.project / 'managed_components'
        return path if path.is_dir() else None

    @property
    def app_partition_size(self) -> int:
        """这次构建的 app 分区有多大（读工程的 sdkconfig + 分区表 CSV）

        读不到就用 4 MiB 兜底（本工程的默认值），只是个进度百分比，不影响功能。
        """
        csv_name = ''
        try:
            text = (self.project / 'sdkconfig').read_text(encoding='utf-8', errors='replace')
            m = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', text, re.M)
            if m:
                csv_name = m.group(1)
        except OSError:
            pass
        for candidate in ([csv_name] if csv_name else []) + ['partitions.csv',
                                                             'partitions-16MiB.csv']:
            path = self.project / candidate
            if not candidate or not path.is_file():
                continue
            size = _app_size_from_csv(path)
            if size:
                return size
        return DEFAULT_APP_PARTITION_SIZE

    def project_problems(self) -> list[str]:
        """检查这个目录像不像一个 ESP-IDF 工程"""
        problems = []
        if not self.project.is_dir():
            return [f'目录不存在：{self.project}']
        if not (self.project / 'CMakeLists.txt').is_file():
            problems.append('没有 CMakeLists.txt，不像是 ESP-IDF 工程')
        elif 'project(' not in (self.project / 'CMakeLists.txt').read_text(
                encoding='utf-8', errors='replace'):
            problems.append('CMakeLists.txt 里没有 project(...)，不像是 ESP-IDF 工程')
        if not (self.project / 'main').is_dir():
            problems.append('没有 main/ 目录（ESP-IDF 工程一般都有）')
        return problems

    def project_name(self) -> str:
        try:
            text = (self.project / 'CMakeLists.txt').read_text(encoding='utf-8', errors='replace')
            m = re.search(r'^\s*project\s*\(\s*([A-Za-z0-9_\-\.]+)', text, re.M)
            if m:
                return m.group(1)
        except OSError:
            pass
        return self.project.name

    def needs_component_manager(self) -> bool:
        """这个工程是不是非得开组件管理器才能编

        情况：工程里写了 idf_component.yml 声明依赖，但 managed_components/
        里一个都没有 —— 说明依赖还得联网下。
        """
        if self.managed_components:
            return False
        for yml in (self.project / 'main' / 'idf_component.yml',
                    self.project / 'idf_component.yml'):
            if yml.is_file():
                return True
        return False

    # -- 工具链 --

    def tool_version(self, name: str) -> str:
        """tools.json 里推荐的版本号"""
        try:
            data = json.loads(self.tools_json.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return ''
        for tool in data.get('tools', []):
            if tool.get('name') != name:
                continue
            versions = tool.get('versions') or []
            if versions:
                for entry in versions:
                    if entry.get('status') == 'recommended':
                        return entry.get('name', '')
                return versions[0].get('name', '')
            return tool.get('version', '')
        return ''

    def tool_path(self, name: str) -> Path | None:
        """返回某个工具实际存在的版本目录"""
        base = self.idf_tools_dir / 'tools' / name
        if not base.is_dir():
            return None
        wanted = self.tool_version(name)
        if wanted and (base / wanted).is_dir():
            return base / wanted
        # 版本对不上就退而求其次，取唯一/最新的一个
        candidates = sorted((p for p in base.iterdir() if p.is_dir()),
                            key=lambda p: p.name, reverse=True)
        return candidates[0] if candidates else None

    def tool_export_entries(self, name: str) -> tuple[list[str], dict[str, str]]:
        """读 tools.json，拿到这个工具要加到 PATH 的哪些子目录、要设哪些环境变量"""
        paths: list[str] = []
        variables: dict[str, str] = {}
        try:
            data = json.loads(self.tools_json.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            data = {}
        for tool in data.get('tools', []):
            if tool.get('name') != name:
                continue
            for parts in tool.get('export_paths') or []:
                joined = os.path.join(*parts) if parts else ''
                paths.append(joined)
            for key, value in (tool.get('export_vars') or {}).items():
                if isinstance(value, dict):
                    value = value.get('win64') or value.get('any') or ''
                    if isinstance(value, list):
                        value = value[0] if value else ''
                variables[key] = str(value)
            break

        tool_dir = self.tool_path(name)
        resolved_paths = []
        for rel in paths:
            resolved_paths.append(str(tool_dir / rel) if rel else str(tool_dir))
        resolved_vars = {
            k: v.replace('${TOOL_PATH}', str(tool_dir)).replace('\\', os.sep)
            for k, v in variables.items()
        }
        return resolved_paths, resolved_vars

    # -- 体检 --

    def missing_required_tools(self) -> list[str]:
        return [name for name in REQUIRED_TOOLS if self.tool_path(name) is None]

    def check_essentials(self) -> list[str]:
        """返回缺失的关键文件列表（空列表代表沙箱完整）

        先看平台对不对 —— 在 Linux 上打开一个 Windows 沙箱，缺的东西会列一长串，
        但真正的原因只有一个，先说清楚那个。
        """
        problem = self.platform_problem()
        if problem:
            return [problem]

        missing = []
        if self.host == 'windows' and not self.python_exe.is_file():
            # Linux/macOS 不打包 Python，用系统自带的那份
            missing.append(f'便携 Python        {self.python_exe}')
        if not self.idf_py.is_file():
            missing.append(f'ESP-IDF            {self.idf_py}')
        for name in self.missing_required_tools():
            missing.append(f'工具链 {name:<12} {self.idf_tools_dir / "tools" / name}')
        return missing

    def essence_ok(self) -> bool:
        return not self.check_essentials()

    def is_complete(self) -> bool:
        if not self.essence_ok():
            return False
        if not self.venv_python.is_file():
            return False
        return True

    # -- 环境变量 --

    def build_env(self, extra: dict | None = None, component_manager: bool | None = None) -> dict:
        """拼出一套干净的环境变量

        component_manager=False 时关掉组件管理器：依赖已经在
        ``managed_components/`` 里备好了，关掉它就不用联网、启动也更快。
        想用官方那套（联网校验组件）就设环境变量 ``IDF_SANDBOX_COMPONENT_MANAGER=1``。
        """
        if component_manager is None:
            component_manager = os.environ.get('IDF_SANDBOX_COMPONENT_MANAGER', '') == '1'
        env = dict(os.environ)

        # 先把可能干扰的变量清掉（客户机上也许装过别的 ESP-IDF）
        for key in ('IDF_PATH', 'IDF_TOOLS_PATH', 'IDF_PYTHON_ENV_PATH', 'ESP_ROM_ELF_DIR',
                    'OPENOCD_SCRIPTS', 'PYTHON', 'PYTHONPATH', 'ESPTOOL_WRAPPER',
                    'IDF_COMPONENT_MANAGER', 'IDF_CCACHE_ENABLE', 'IDF_TARGET'):
            env.pop(key, None)

        # PATH 里先放沙箱自己的东西（Windows 是 Scripts\，Linux 是 bin/）
        if self.host == 'windows':
            path_parts: list[str] = [
                str(self.venv_dir / 'Scripts'),
                str(self.python_dir),
                str(self.python_dir / 'Scripts'),
            ]
        else:
            path_parts = [
                str(self.venv_dir / 'bin'),
                str(self.python_dir / 'bin'),
            ]
        for name in REQUIRED_TOOLS + OPTIONAL_TOOLS:
            if self.tool_path(name) is None:
                continue
            for part in self.tool_export_entries(name)[0]:
                path_parts.append(part)
            for key, value in self.tool_export_entries(name)[1].items():
                env[key] = value

        env['IDF_PATH'] = str(self.idf_dir)
        env['IDF_TOOLS_PATH'] = str(self.idf_tools_dir)
        env['IDF_PYTHON_ENV_PATH'] = str(self.venv_dir)
        env['ESP_IDF_VERSION'] = self.idf_version_short
        env['PYTHON'] = str(self.venv_python)
        env['PYTHONUTF8'] = '1'
        env['PYTHONIOENCODING'] = 'utf-8'
        env['PYTHONDONTWRITEBYTECODE'] = '1'
        env['IDF_CCACHE_ENABLE'] = '0'          # 沙箱里没打包 ccache
        env['IDF_PYTHON_CHECK_CONSTRAINTS'] = 'no'
        env['IDF_COMPONENT_MANAGER'] = '1' if component_manager else '0'
        env['PYTHONHOME'] = ''
        env['PYTHONPATH'] = ''
        env['TERM'] = env.get('TERM', 'xterm-256color')

        env['PATH'] = os.pathsep.join(path_parts + [env.get('PATH', '')])

        # 临时目录也搬进沙箱。有些电脑（公司电脑、装了杀毒软件的）不让往系统 %TEMP% 写，
        # 交叉编译器一旦建不了临时文件就会报 "Cannot create temporary file"；
        # 顺带好处是沙箱真的就是一个盒子，不在系统里留垃圾。
        try:
            tmp_dir = self.root / 'tmp'
            tmp_dir.mkdir(parents=True, exist_ok=True)
            env['TMP'] = str(tmp_dir)       # Windows
            env['TEMP'] = str(tmp_dir)
            env['TMPDIR'] = str(tmp_dir)    # Linux / macOS
            _tidy_tmp(tmp_dir)
        except OSError:
            pass

        if extra:
            env.update({k: str(v) for k, v in extra.items()})
        return env

    def idf_command(self, args: list[str]) -> list[str]:
        """拼出运行 idf.py 的命令行"""
        return [str(self.venv_python), str(self.idf_py), *args]


# ---------------------------------------------------------------------------
# 虚拟环境：换电脑后能自动修好
# ---------------------------------------------------------------------------

def ensure_venv(sandbox: Sandbox, reporter: Reporter | None = None) -> Path:
    """确保虚拟环境可用（Windows / Linux 通用）

    虚拟环境只是一个"壳"：``pyvenv.cfg`` 里记着沙箱 Python 的绝对路径。
    整个沙箱换了位置（换电脑、换盘符、插到别的系统上）这个路径就失效了，
    所以每次运行都重写一遍；真丢了就用沙箱 Python 重建一个 ——
    ``--system-site-packages`` 让它直接复用沙箱 Python 里已经装好的依赖包，
    所以重建是**完全离线**的，Windows 上 0.3 秒，Linux 上差不多。
    """
    say = reporter or Reporter(color=False)
    base_python = sandbox.python_exe
    if not base_python.is_file() and sandbox.host != 'windows':
        # Linux/macOS 没打包 Python 时，用当前这个解释器当母体
        base_python = Path(sys.executable)

    if not base_python.is_file():
        raise SandboxError(
            f'沙箱自带的 Python 不见了：{sandbox.python_exe}\n'
            f'（{sandbox.host} 版）跑一次修复试试：'
            f'python {sandbox.root / "start.py"} prepare --download')

    # 非 Windows 上不在这儿"空手"建环境 —— 那需要联网装依赖，
    # 交给 prepare.py 干（start.py 通常已经自动跑过了）
    if sandbox.host != 'windows' and not sandbox.venv_python.is_file():
        raise SandboxError(
            f'{sandbox.host} 上的 Python 环境还没准备好：{sandbox.venv_dir}\n'
            f'跑一次（要联网，只做一次）：\n'
            f'    python3 {sandbox.root / "start.py"} prepare --download')

    pyvenv = sandbox.venv_dir / 'pyvenv.cfg'
    need_create = not sandbox.venv_python.is_file()
    if need_create and sandbox.venv_dir.exists():
        say.info('虚拟环境不完整，正在重建……')
        shutil.rmtree(sandbox.venv_dir, ignore_errors=True)

    if need_create:
        say.info('正在创建虚拟环境（首次运行只做一次，几秒钟）')
        result = subprocess.run(
            [str(base_python), '-m', 'venv', '--system-site-packages', '--without-pip',
             str(sandbox.venv_dir)],
            capture_output=True, text=True, encoding='utf-8', errors='replace',
            creationflags=NO_WINDOW)
        if result.returncode != 0 or not sandbox.venv_python.is_file():
            raise SandboxError('创建虚拟环境失败：\n' + (result.stdout or '') + (result.stderr or ''))

    # 每次都把 home 指向当前沙箱，换了电脑/盘符也能用
    desired = {
        'home': str(base_python.parent),
        'include-system-site-packages': 'true',
        'version': _python_version(base_python),
        'executable': str(base_python),
        'command': f'{base_python} -m venv --system-site-packages --without-pip {sandbox.venv_dir}',
    }
    try:
        existing = {}
        if pyvenv.is_file():
            for raw in pyvenv.read_text(encoding='utf-8', errors='replace').splitlines():
                if '=' in raw:
                    key, _, value = raw.partition('=')
                    existing[key.strip()] = value.strip()
        if any(existing.get(k) != v for k, v in desired.items()):
            pyvenv.parent.mkdir(parents=True, exist_ok=True)
            pyvenv.write_text(''.join(f'{k} = {v}\n' for k, v in desired.items()), encoding='utf-8')
    except OSError as exc:
        raise SandboxError(f'写 pyvenv.cfg 失败：{exc}') from exc

    return sandbox.venv_python


def _python_version(python_exe: Path) -> str:
    try:
        result = subprocess.run(
            [str(python_exe), '-c', 'import sys;print("%d.%d.%d" % sys.version_info[:3])'],
            capture_output=True, text=True, encoding='utf-8', errors='replace',
            timeout=30, creationflags=NO_WINDOW)
        return result.stdout.strip() or '3.11.2'
    except Exception:
        return '3.11.2'


# ---------------------------------------------------------------------------
# 跑子进程
# ---------------------------------------------------------------------------

class RunResult:
    def __init__(self, code: int, seconds: float, tail: list[str]):
        self.code = code
        self.seconds = seconds
        self.tail = tail

    @property
    def ok(self) -> bool:
        return self.code == 0


def _consume_lines(buffer: bytes) -> tuple[list[tuple[str, bool]], bytes]:
    """把缓冲区切成若干行

    返回 ``([(文本, 是不是"原地刷新")], 剩下的半行)``。

    这里要分清两种回车：

    * ``\\r\\n`` —— 就是 Windows 的普通换行，该老老实实另起一行
    * 光秃秃一个 ``\\r`` —— esptool 烧录进度条那种原地刷新，屏幕上要盖掉上一行
    """
    lines: list[tuple[str, bool]] = []
    start = 0
    index = 0
    size = len(buffer)
    while index < size:
        byte = buffer[index]
        if byte == 0x0D:                       # \r
            if index + 1 == size:
                break                          # 可能是 \r\n 的前半截，等下一块数据
            crlf = buffer[index + 1] == 0x0A
            lines.append((buffer[start:index].decode('utf-8', 'replace'), not crlf))
            index += 1
            if crlf:
                index += 1
            start = index
        elif byte == 0x0A:                     # \n
            lines.append((buffer[start:index].decode('utf-8', 'replace'), False))
            index += 1
            start = index
        else:
            index += 1
    return lines, buffer[start:]


def run_streamed(cmd: list[str], env: dict, cwd: Path, reporter: Reporter,
                 keep_tail: int = 40) -> RunResult:
    """跑一个命令，输出实时打到屏幕上、同时写进日志"""
    reporter.line(reporter.c('  $ ' + ' '.join(cmd), 'dim'))
    start = time.time()
    tail: list[str] = []
    creationflags = NO_WINDOW
    if os.name == 'nt':
        creationflags |= getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)

    try:
        proc = subprocess.Popen(
            cmd, env=env, cwd=str(cwd),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, bufsize=0, creationflags=creationflags)
    except OSError as exc:
        reporter.error(f'启动失败：{exc}')
        return RunResult(-1, 0.0, tail)

    buffer = b''
    stdout = proc.stdout
    assert stdout is not None
    while True:
        chunk = stdout.read(4096)
        if not chunk:
            break
        buffer += chunk
        lines, buffer = _consume_lines(buffer)
        for text, in_place in lines:
            text = text.rstrip('\r')
            if not in_place:
                reporter.raw(text + '\n')
            elif text:
                # esptool 的进度条：\r 开头，原地刷新同一行
                reporter.raw_replace(text)
            # in_place 但是空串（进度条前面那个光秃秃的 \r）：什么都不用做
            if text:
                tail.append(text)
                if len(tail) > keep_tail:
                    del tail[0]
    if buffer:
        text = buffer.decode('utf-8', 'replace').rstrip('\r')
        reporter.raw(text + '\n')
        tail.append(text)

    proc.wait()
    return RunResult(proc.returncode, time.time() - start, tail)


# ---------------------------------------------------------------------------
# 串口
# ---------------------------------------------------------------------------

class SerialPortInfo:
    def __init__(self, device: str, description: str, hwid: str = '', vid: int = 0):
        self.device = device
        self.description = description or ''
        self.hwid = hwid or ''
        self.vid = vid

    @property
    def friendly(self) -> str:
        return self.description.strip() or '未知设备'

    @property
    def is_linux_device(self) -> bool:
        return self.device.startswith('/dev/')

    @property
    def is_virtual(self) -> bool:
        """看着不像"插上去的开发板"就当成虚拟口

        * Windows: 没有 USB VID 的（蓝牙虚拟串口之类）
        * Linux: /dev/ttyS* 是主板自带串口，开发板不会出现在那儿
        """
        if self.is_linux_device:
            return self.device.startswith('/dev/ttyS')
        return self.vid == 0

    @property
    def note(self) -> str:
        if self.is_linux_device:
            if self.device.startswith('/dev/ttyACM'):
                return 'USB CDC（很可能就是开发板）'
            if self.device.startswith('/dev/ttyUSB'):
                return 'USB 转串口（很可能就是开发板）'
            if self.device.startswith('/dev/ttyS'):
                return '主板自带串口，一般不是开发板'
            return ''
        if self.vid == 0x303A:
            return 'ESP32 原生 USB'
        if self.vid in KNOWN_USB_VIDS:
            return KNOWN_USB_VIDS[self.vid]
        if self.is_virtual:
            return '虚拟串口（蓝牙/内置），一般不是开发板'
        return ''

    @property
    def rank(self) -> int:
        """越小越像开发板

        Linux 上不看 VID（常常是空的），直接按设备名判断：
        ``/dev/ttyACM*``（USB CDC）和 ``/dev/ttyUSB*``（USB 转串口）才是开发板，
        ``/dev/ttyS*`` 是主板串口 —— 之前没区分，结果一上来就挑了 /dev/ttyS0。
        """
        if self.is_linux_device:
            if self.device.startswith('/dev/ttyACM'):
                return 0
            if self.device.startswith('/dev/ttyUSB'):
                return 1
            if self.device.startswith('/dev/serial/by-id/'):
                return 0
            if self.device.startswith('/dev/ttyS'):
                return 9
            return 3
        if self.vid == 0x303A:
            return 0
        if self.vid in KNOWN_USB_VIDS:
            return 1
        if self.vid:
            return 2
        return 3

    def __repr__(self) -> str:
        return f'<{self.device} {self.friendly}>'


def list_serial_ports() -> list[SerialPortInfo]:
    """扫串口（依赖 pyserial，沙箱自带的）"""
    ports: list[SerialPortInfo] = []
    try:
        from serial.tools import list_ports
    except ImportError:
        return ports

    for item in list_ports.comports():
        vid = 0
        if item.vid is not None:
            vid = int(item.vid)
        ports.append(SerialPortInfo(item.device, item.description or '', item.hwid or '', vid))
    ports.sort(key=lambda p: (p.rank, _port_number(p.device)))
    return ports


def _port_number(device: str) -> int:
    m = re.search(r'(\d+)\s*$', device or '')
    return int(m.group(1)) if m else 9999


def show_ports(ports: list[SerialPortInfo] | None = None) -> None:
    """打印串口列表，附上"没找到怎么办"的提示"""
    if ports is None:
        ports = list_serial_ports()
    if not ports:
        print('  没扫到串口。按顺序检查：')
        print('    1) USB 线插好了吗（要能传数据的线，不是纯充电线）')
        print('    2) 板子上的电源灯亮了吗')
        if os.name == 'nt':
            print(r'    3) 串口驱动装了吗 —— 见 .idf-sandbox\drivers\ 里的安装包')
        else:
            print('    3) 在不在 dialout 组？  sudo usermod -aG dialout $USER  （之后要重新登录）')
            print('       插上板子后 dmesg | tail 看看有没有 ttyUSB0 / ttyACM0 冒出来')
        return
    print(f'  找到 {len(ports)} 个串口：')
    for index, info in enumerate(ports, 1):
        note = f'   <- {info.note}' if info.note else ''
        print(f'   {index:>2}  {info.device:<8} {info.friendly}{note}')
    if all(p.is_virtual for p in ports):
        print('  这些看起来都是虚拟串口，没看到开发板。板子插上后重新扫一次。')


def choose_port(preferred: str = '', interactive: bool | None = None) -> str:
    """挑一个串口：参数 > 上次记住的 > 唯一的那个 > 让用户选"""
    if interactive is None:
        interactive = sys.stdin.isatty() if hasattr(sys.stdin, 'isatty') else False

    ports = list_serial_ports()
    if preferred:
        if any(p.device.lower() == preferred.lower() for p in ports):
            return preferred
        print(f'  串口 {preferred} 不存在，当前可用的是：')
        show_ports(ports)
        return ''
    if not ports:
        show_ports(ports)
        return ''
    if len(ports) == 1:
        return ports[0].device

    saved = str(load_state().get('port', ''))
    if saved and any(p.device == saved for p in ports):
        print(f'  用上次记住的 {saved}（想换一个就加 -p 参数）')
        return saved

    if not interactive:
        return ports[0].device

    show_ports(ports)
    try:
        answer = input(f'  用哪个串口？[1-{len(ports)}，回车=1] ').strip()
    except (EOFError, KeyboardInterrupt):
        # 没有键盘可读（被重定向、或者无人值守），就默认挑排最前的那个
        print()
        return ports[0].device
    if not answer:
        return ports[0].device
    try:
        index = int(answer)
    except ValueError:
        return ports[0].device
    return ports[index - 1].device if 1 <= index <= len(ports) else ports[0].device


# ---------------------------------------------------------------------------
# 记住上次用的串口/波特率（存在用户目录，因为串口号是"这台电脑"的事）
# ---------------------------------------------------------------------------

def load_state(sandbox: Sandbox | None = None) -> dict:
    sb = sandbox or Sandbox()
    try:
        return json.loads(sb.state_file.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_state(data: dict, sandbox: Sandbox | None = None) -> None:
    sb = sandbox or Sandbox()
    try:
        sb.state_file.parent.mkdir(parents=True, exist_ok=True)
        current = load_state(sb)
        current.update(data)
        sb.state_file.write_text(json.dumps(current, ensure_ascii=False, indent=2), encoding='utf-8')
    except OSError:
        pass


# ---------------------------------------------------------------------------
# 杂项
# ---------------------------------------------------------------------------

def sandbox_root() -> Path:
    """沙箱自己的位置

    这个文件就在 ``.idf-sandbox/tools/core.py``，所以上一级是沙箱、
    再上一级是工程。沙箱在哪儿是"推"出来的，不依赖当前工作目录。
    """
    return Path(__file__).resolve().parents[1]


def default_project_dir() -> Path:
    """沙箱旁边那个工程 —— 也就是 ``.idf-sandbox`` 所在的目录"""
    return Path(__file__).resolve().parents[2]


def desktop_dir() -> Path:
    """桌面目录

    别直接拼 ``%USERPROFILE%\\Desktop``：桌面可能被 OneDrive 接管、
    也可能被组策略改到别处。问系统最准，问不到再退回拼路径。
    """
    if os.name == 'nt':
        try:
            import ctypes
            buf = ctypes.create_unicode_buffer(260)
            # CSIDL_DESKTOPDIRECTORY = 0x10
            if ctypes.windll.shell32.SHGetFolderPathW(None, 0x10, None, 0, buf) == 0:
                if buf.value and Path(buf.value).is_dir():
                    return Path(buf.value)
        except Exception:
            pass
    profile = os.environ.get('USERPROFILE') or str(Path.home())
    for name in ('Desktop', 'OneDrive\\Desktop', 'OneDrive/Desktop'):
        candidate = Path(profile) / name
        if candidate.is_dir():
            return candidate
    return Path(profile)


def open_log(sandbox: Sandbox, prefix: str) -> Reporter:
    """开一个带时间戳的日志文件

    日志优先放在**被构建工程**的 ``logs/`` 下；如果那个目录写不了
    （只读、网络盘、别人的工程），就退回到沙箱自己的 ``logs/``。
    """
    stamp = time.strftime('%Y%m%d-%H%M%S')
    for directory in (sandbox.log_dir, sandbox.root / 'logs'):
        try:
            directory.mkdir(parents=True, exist_ok=True)
            log_path = directory / f'{prefix}-{stamp}.log'
            _rotate_logs(directory, prefix, keep=10)
            return Reporter(log_file=log_path)
        except OSError:
            continue
    return Reporter(log_file=None)   # 实在写不了就不写日志，照样干活


def _rotate_logs(log_dir: Path, prefix: str, keep: int) -> None:
    try:
        old = sorted(log_dir.glob(f'{prefix}-*.log'), key=lambda p: p.stat().st_mtime, reverse=True)
        for path in old[keep:]:
            path.unlink()
    except OSError:
        pass


def pause_if_needed(enabled: bool, reporter: Reporter, message: str = '按回车键关闭窗口……') -> None:
    """双击运行时，出错了别让窗口一闪而过"""
    if not enabled:
        return
    try:
        reporter.line('')
        input(reporter.c('  ' + message, 'dim'))
    except (EOFError, KeyboardInterrupt):
        pass


def print_path_warning(sandbox: Sandbox, reporter: Reporter) -> None:
    """路径里有中文时提醒一句（交叉工具链对非 ASCII 路径不太友好）"""
    for label, path in (('工程目录', sandbox.project), ('沙箱目录', sandbox.root)):
        if not is_ascii(str(path)):
            reporter.warn(f'{label}路径里有非 ASCII 字符（中文/空格等）：{path}')
            reporter.hint('ESP32 的交叉工具链在非英文路径下可能报奇怪的错。')
            reporter.hint('如果编译失败，把整个工程文件夹挪到纯英文路径，例如 D:\\esp32_smart_home')
            return
