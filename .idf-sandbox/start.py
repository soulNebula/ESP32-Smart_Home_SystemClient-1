#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""ESP32 沙箱总入口 —— Windows 和 Linux 两套工具链装在一起，按系统选用

启动方式（先用自己系统对应的那条）：

    Windows:   python .idf-sandbox\start.py
    Linux:     python3 .idf-sandbox/start.py

打开以后它会：
    1. 认出你当前是什么系统（Windows / Linux）
    2. 如果沙箱里两套工具链都装了，第一次会**问你要用哪一套**（之后记住）
    3. 用对应那套的 Python 把自己重新启动一遍
    4. 把活交给 tools/<脚本>.py

任务（两个系统一模一样）：

    python start.py            图形控制台（默认，也可以直接双击 start.py）
    python start.py run        编译 + 烧录 + 看串口
    python start.py build      只编译
    python start.py flash      只烧录
    python start.py monitor    只看串口
    python start.py doctor     环境体检
    python start.py prepare    准备 / 修复沙箱（缺哪套就装哪套）
    python start.py --help     看全部

想跳过询问、直接指定用哪套工具链：

    python start.py --os windows
    python start.py --os linux
    python start.py --os auto        （默认：按当前系统）

## 为什么一个沙箱里能装两套

工具链是可执行文件，Windows 的是 PE（.exe），Linux 的是 ELF，**不能互相运行**，
所以两套各放各的目录、互不干扰：

    .idf-sandbox/
    ├── idf/                  ESP-IDF 源码（两边共用，纯 Python + C）
    ├── python/  penv/        Windows 的 Python 和虚拟环境
    ├── idf_tools/            Windows 的工具链（*.exe）
    ├── python-linux/  penv-linux/
    ├── idf_tools-linux/      Linux 的工具链（ELF）
    └── drivers/              USB 串口驱动（只有 Windows 需要装）

## 环境变量

    ESP32_NO_RELAUNCH=1    不要切到沙箱解释器（排查问题时用）
    ESP32_PLATFORM=linux   等同于 --os linux

## 为什么写得这么"老土"

不用 f-string、不用类型注解、不用海象运算符、连 print 都从 __future__ 里引 ——
这样连 Python 2 都能把这个文件解析开，从而有机会弹一句"版本太低"，
而不是让用户对着一堆 SyntaxError 发呆。
"""

from __future__ import print_function

import json
import os
import sys
import time

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

MIN_PYTHON = (3, 9)
DEFAULT_TASK = 'gui'
PLATFORM_NAMES = {
    'windows': 'Windows',
    'linux': 'Linux',
    'macos': 'macOS',
}

# 任务名 -> (tools 下的文件名, 是不是图形界面, 控制台标题)
TASKS = {
    'gui':     ('gui.py',     True,  'ESP32 控制台'),
    'run':     ('run.py',     False, 'ESP32 一键编译烧录'),
    'build':   ('build.py',   False, 'ESP32 只编译'),
    'flash':   ('flash.py',   False, 'ESP32 烧录'),
    'monitor': ('monitor.py', False, 'ESP32 串口监视器'),
    'doctor':  ('doctor.py',  False, 'ESP32 环境体检'),
    'prepare': ('prepare.py', False, 'ESP32 准备 / 修复沙箱'),
}

USAGE = r"""ESP32 沙箱总入口（start.py 就放在 .idf-sandbox\ 里面）

用法：
    python  .idf-sandbox\start.py [--os 平台] [任务] [任务参数...]
    python3 .idf-sandbox/start.py [--os 平台] [任务] [任务参数...]

任务：
    gui        图形控制台（默认）
    run        编译 + 烧录 + 看串口
    build      只编译
    flash      只烧录
    monitor    只看串口
    doctor     环境体检
    prepare    准备 / 修复沙箱

--os 用哪套工具链（不写就按当前系统自动选；两套都装了会问你一次）：
    --os windows | --os linux | --os auto

例子：
    python  .idf-sandbox\start.py
    python  .idf-sandbox\start.py build --clean
    python3 .idf-sandbox/start.py --os linux
    python3 .idf-sandbox/start.py run -p /dev/ttyUSB0
"""


# ---------------------------------------------------------------------------
# 平台
# ---------------------------------------------------------------------------

def host_platform():
    """当前操作系统"""
    if sys.platform.startswith('win'):
        return 'windows'
    if sys.platform.startswith('linux'):
        return 'linux'
    if sys.platform == 'darwin':
        return 'macos'
    return sys.platform


def sandbox_dir():
    """沙箱自己的目录 —— 本文件就在这里面"""
    return os.path.dirname(os.path.abspath(__file__))


def project_dir():
    """沙箱旁边那个工程，也就是 .idf-sandbox 的上级目录"""
    return os.path.dirname(sandbox_dir())


def tools_dir():
    return os.path.join(sandbox_dir(), 'tools')


def tools_root(plat):
    """那个平台的工具链放在哪"""
    if plat == 'windows':
        return os.path.join(sandbox_dir(), 'idf_tools')
    return os.path.join(sandbox_dir(), 'idf_tools-' + plat)


def venv_dir(plat):
    if plat == 'windows':
        return os.path.join(sandbox_dir(), 'penv')
    return os.path.join(sandbox_dir(), 'penv-' + plat)


def venv_python(plat):
    if plat == 'windows':
        return os.path.join(venv_dir(plat), 'Scripts', 'python.exe')
    return os.path.join(venv_dir(plat), 'bin', 'python')


def sandbox_python(plat, graphical):
    """沙箱自带的解释器（不是虚拟环境那个）"""
    if plat == 'windows':
        name = 'pythonw.exe' if graphical else 'python.exe'
        return os.path.join(sandbox_dir(), 'python', name)
    return os.path.join(sandbox_dir(), 'python-' + plat, 'bin', 'python3')


def installed_platforms():
    """沙箱里已经装好了哪几套工具链"""
    found = []
    for plat in ('windows', 'linux', 'macos'):
        marker = os.path.join(tools_root(plat), 'tools', 'xtensa-esp-elf')
        if os.path.isdir(marker):
            found.append(plat)
    return found


def tools_ready(plat):
    """这套工具链齐了吗（编译器在不在）"""
    root = os.path.join(tools_root(plat), 'tools', 'xtensa-esp-elf')
    if not os.path.isdir(root):
        return False
    for _dirpath, _dirs, files in os.walk(root):
        for name in files:
            if name.startswith('xtensa-esp32s3-elf-gcc'):
                return True
    return False


def platform_label(plat):
    return PLATFORM_NAMES.get(plat, plat)


# ---------------------------------------------------------------------------
# 记一下用户选过哪套（存在用户目录，不是沙箱里 —— "你是哪个系统"是这台电脑的事）
# ---------------------------------------------------------------------------

def _state_path():
    base = (os.environ.get('LOCALAPPDATA') or os.environ.get('XDG_STATE_HOME')
            or os.environ.get('TEMP') or os.path.expanduser('~'))
    return os.path.join(base, 'esp32-smarthome', 'state.json')


def state_get(key, default=None):
    try:
        with open(_state_path(), encoding='utf-8') as fh:
            return json.load(fh).get(key, default)
    except Exception:
        return default


def state_put(key, value):
    try:
        path = _state_path()
        try:
            with open(path, encoding='utf-8') as fh:
                data = json.load(fh)
        except Exception:
            data = {}
        data[key] = value
        directory = os.path.dirname(path)
        if not os.path.isdir(directory):
            os.makedirs(directory)
        with open(path, 'w', encoding='utf-8') as fh:
            json.dump(data, fh, ensure_ascii=False, indent=2)
    except Exception:
        pass       # 记不住就算了，不影响干活


# ---------------------------------------------------------------------------
# 报错 / 交互
# ---------------------------------------------------------------------------

def _has_console():
    """到底有没有地方能看见 print 出来的字

    Windows：看进程有没有挂控制台（pythonw / 分离进程是没有的）。
    别的系统：stderr 还在就算有。
    注意这里**不能**只看 isatty() —— 输出被重定向到管道时 isatty() 是 False，
    但控制台明明在，那样会平白弹一个模态对话框出来，把脚本卡死（踩过）。
    """
    try:
        if os.name == 'nt':
            import ctypes
            return bool(ctypes.windll.kernel32.GetConsoleWindow())
    except Exception:
        pass
    try:
        return sys.stderr is not None
    except Exception:
        return False


def say(text, title='ESP32 沙箱'):
    """出事了：有控制台就打印，没控制台（双击/pythonw）才补一个弹框

    双击 .py 时控制台一闪就没了，光 print 等于没提示；pythonw 干脆没有 stderr。
    所以这两种情况必须弹框。反过来，命令行里敲错了就只打印，别弹框烦人 ——
    而且弹框是模态的，在没人点的场合（后台、管道）会一直挂着。
    """
    lines = text.replace('\n', os.linesep)
    if not _has_console():
        try:
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, lines, title, 0x10)   # MB_ICONERROR
        except Exception:
            pass
    try:
        sys.stderr.write(lines + os.linesep)
    except Exception:
        pass


def set_console_title(text):
    """给控制台窗口起个像样的标题（Windows 专有，别的系统上安静失败）"""
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleTitleW(text)
    except Exception:
        pass


def _interactive():
    try:
        return (sys.stdin is not None and sys.stdin.isatty()
                and sys.stdout is not None and sys.stdout.isatty())
    except Exception:
        return False


def _read_line(seconds):
    """读一行输入，超时就返回 None

    为什么不直接用 input()：它是**死等**的。如果这个进程是被人双击起来的、
    或者窗口被最小化/隐藏了，没人看见那行提示，脚本就永远卡在那一句上 ——
    用户看到的是"双击了没反应"。所以必须有超时兜底。
    """
    deadline = time.time() + seconds
    if os.name == 'nt':
        try:
            import msvcrt
            buf = []
            while time.time() < deadline:
                if not msvcrt.kbhit():
                    time.sleep(0.05)
                    continue
                ch = msvcrt.getwch()
                if ch in ('\r', '\n'):
                    sys.stdout.write('\n')
                    sys.stdout.flush()
                    return ''.join(buf)
                if ch == '\x03':
                    raise KeyboardInterrupt
                if ch in ('\x08', '\x7f'):
                    if buf:
                        buf.pop()
                        sys.stdout.write('\b \b')
                        sys.stdout.flush()
                    continue
                if ch in ('\x00', '\xe0'):
                    msvcrt.getwch()          # 方向键之类，吃掉第二个字节
                    continue
                buf.append(ch)
                sys.stdout.write(ch)
                sys.stdout.flush()
            sys.stdout.write('\n')
            sys.stdout.flush()
            return None
        except ImportError:
            pass
    try:
        import select
        ready, _w, _x = select.select([sys.stdin], [], [], seconds)
        if not ready:
            sys.stdout.write('\n')
            sys.stdout.flush()
            return None
        return sys.stdin.readline().strip()
    except Exception:
        return ''


def ask_platform(host, available, seconds=15):
    """两套都装了，问一次用哪个（超时就按当前系统）；问完记住"""
    print('')
    print('这个沙箱里装了 Windows 和 Linux 两套工具链：')
    for index, plat in enumerate(available, 1):
        mark = '   <- 你当前就是这个系统' if plat == host else ''
        print('    %d) %-8s%s' % (index, platform_label(plat), mark))
    print('')
    print('两套的可执行文件格式不一样（Windows 是 .exe，Linux 是 ELF），')
    print('只能用跟当前系统匹配的那套。')
    print('')
    sys.stdout.write('用哪套？[回车 = %s，%d 秒不答就按它走] '
                     % (platform_label(host), seconds))
    sys.stdout.flush()

    answer = _read_line(seconds)
    if answer is None:
        print('（没等到输入，就用 %s 了）' % platform_label(host))
        return host
    answer = answer.strip()
    if not answer:
        return host
    if answer.isdigit():
        index = int(answer)
        if 1 <= index <= len(available):
            return available[index - 1]
    answer = answer.lower()
    if answer in available:
        return answer
    print('没听懂 %r，就用 %s 了。' % (answer, platform_label(host)))
    return host


# ---------------------------------------------------------------------------
# 启动
# ---------------------------------------------------------------------------

def same_file(a, b):
    try:
        return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))
    except Exception:
        return False


def relaunch_with_sandbox(plat, task, args, graphical):
    """换成沙箱里的解释器再跑一遍

    换成功了就一去不回；换不过去就换个方式拉起子进程。
    """
    if os.environ.get('ESP32_NO_RELAUNCH'):
        return False

    target = sandbox_python(plat, graphical)
    if not os.path.isfile(target):
        return False                      # 沙箱 Python 不在，用当前的凑合
    if same_file(target, sys.executable):
        return False                      # 已经就是它，别再套一层

    argv = [target, os.path.abspath(__file__), '--os', plat, task] + list(args)
    try:
        try:
            sys.stdout.flush()
            sys.stderr.flush()
        except Exception:
            pass
        os.execv(target, argv)
        return True                       # 正常情况走不到这儿
    except Exception:
        pass

    # os.execv 偶尔不灵。退回子进程方式 —— 但**不能 call() 等它**：
    # 图形界面是要一直开着的，父进程等下去就等于"终端关了界面也跟着死"。
    try:
        import subprocess
        flags = 0
        if graphical and os.name == 'nt':
            flags = getattr(subprocess, 'DETACHED_PROCESS', 0x00000008)
        child = subprocess.Popen(argv, creationflags=flags, close_fds=True)
        if graphical:
            return sys.exit(0)            # 界面交给子进程，父进程功成身退
        return sys.exit(child.wait())
    except Exception:
        return False


def resolve_platform(requested):
    """定下用哪套工具链，返回 (平台, 需要直接返回的退出码或 None)"""
    host = host_platform()
    available = installed_platforms()

    if requested and requested != 'auto':
        if requested != host:
            say('你指定的是 %s 版工具链，但当前系统是 %s。\n\n'
                '两边的可执行文件格式不一样（Windows 是 .exe，Linux 是 ELF），\n'
                '没法在 %s 上运行 %s 的那套。\n\n'
                '在 %s 上请用：--os %s（或者干脆不写 --os，默认就是它）'
                % (platform_label(requested), platform_label(host),
                   platform_label(host), platform_label(requested),
                   platform_label(host), host))
            return requested, 2
        return requested, None

    # 没指定：按当前系统。两套都装了就先问一次（问过就记住）
    if len(available) > 1 and _interactive():
        remembered = state_get('platform')
        if remembered != host:
            picked = ask_platform(host, available)
            state_put('platform', picked)
            if picked != host:
                say('%s 和当前系统（%s）对不上，那套跑不起来。\n\n'
                    '要么选 %s，要么干脆不写 --os —— 脚本自己会挑对的那个。'
                    % (platform_label(picked), platform_label(host),
                       platform_label(host)))
                return picked, 2
    return host, None


def check_tools(plat):
    """这套工具链装好了吗，没装就说清楚怎么装"""
    if tools_ready(plat):
        return True
    if plat != host_platform():
        say('沙箱里没有 %s 版的工具链。\n\n'
            '要装它，得在 %s 上跑：\n'
            '    python3 .idf-sandbox/start.py prepare --download'
            % (platform_label(plat), platform_label(plat)))
        return False
    print('')
    print('[!] 沙箱里还没有 %s 版的工具链。' % platform_label(plat))
    print('')
    print('    装它（第一次要联网，之后就一直离线可用了）：')
    print('        python %s prepare --download'
          % os.path.join('.idf-sandbox', 'start.py'))
    print('')
    return False


def ensure_python_env(plat, task):
    """第一次跑：没有 Python 环境就先自动准备好

    拷来的完整沙箱里什么都有，直接能用；**在线配置**的沙箱只有脚本，
    Python 环境要现建（建 venv + 装 ESP-IDF 依赖，只有第一次要联网）。
    与其让用户先去敲一条 prepare，不如这里顺手做掉 —— 他点一下就该能跑。
    """
    if task in ('prepare', 'doctor'):
        return True
    if os.path.isfile(venv_python(plat)):
        return True
    if not tools_ready(plat):
        return True          # 工具链都没装，让 check_tools 去报，别抢戏

    print('')
    print('第一次在这台电脑上用这个沙箱，先把 Python 环境准备好。')
    print('（要联网，几分钟；只做这一次，以后完全离线）')
    print('')
    try:
        import subprocess
        code = subprocess.call([sys.executable, os.path.abspath(__file__),
                                'prepare', '--download'])
    except Exception as exc:
        say('准备 Python 环境失败：%s\n\n手动跑一次看详细报错：\n'
            '    python .idf-sandbox\\start.py prepare --download' % exc)
        return False
    if code != 0 or not os.path.isfile(venv_python(plat)):
        say('Python 环境还没准备好。\n\n'
            '最常见的原因是没联网。手动跑一次看详细报错：\n'
            '    python .idf-sandbox\\start.py prepare --download')
        return False
    return True


def run(plat, task, args):
    """跑一个任务，返回退出码"""
    if sys.version_info < MIN_PYTHON:
        say('需要 Python %d.%d 或更高版本，当前是 %s。\n\n'
            'Windows 上其实什么都不用装 —— 沙箱里自带 Python，\n'
            '双击 .idf-sandbox\\启动-Windows.cmd 即可；\n'
            'Linux 上用系统自带的 python3 就行。'
            % (MIN_PYTHON[0], MIN_PYTHON[1], sys.version.split()[0]),
            'Python 版本太低')
        return 2

    filename, graphical, title = TASKS[task]
    if not graphical and plat == 'windows':
        set_console_title(title)

    # 请求帮助时不换解释器，不然 pythonw 没有控制台，帮助信息就没了
    wants_help = any(a in ('-h', '--help') for a in args)
    if not wants_help and task != 'prepare':
        relaunch_with_sandbox(plat, task, args, graphical)

    # 到这儿说明解释器定了，可以把工具的目录挂上，正式干活
    here = tools_dir()
    if here not in sys.path:
        sys.path.insert(0, here)

    # gui / doctor / prepare 这三个**即使沙箱不完整也必须能跑起来**。
    # 尤其是 gui：从 GitHub clone 下来时沙箱是空的，用户双击就想看到窗口、
    # 点那个「配置沙箱环境」按钮。要是这里直接拦掉，他只会看到黑窗口里一行字，
    # 连按钮都摸不着 —— 那就谈不上傻瓜式了。
    if task not in ('prepare', 'doctor', 'gui') and not check_tools(plat):
        return 3
    if task != 'gui' and not ensure_python_env(plat, task):
        return 3

    module_name = filename[:-3]           # 'gui.py' -> 'gui'
    try:
        module = __import__(module_name)
    except ImportError as exc:
        say('加载 %s 失败：%s\n\n'
            '多半是沙箱不完整。试试：\n'
            '    python %s prepare --download\n'
            '或者把整个 .idf-sandbox 文件夹重新拷一遍。'
            % (filename, exc, os.path.join('.idf-sandbox', 'start.py')), '启动失败')
        return 2

    return module.main(args)


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    argv = list(argv)

    if argv and argv[0] in ('-h', '--help', 'help', '?'):
        print(USAGE)
        return 0

    # 先摘 --os
    requested = os.environ.get('ESP32_PLATFORM', '') or 'auto'
    if '--os' in argv:
        index = argv.index('--os')
        try:
            requested = argv[index + 1]
        except IndexError:
            requested = 'auto'
        del argv[index:index + 2]

    # 再摘任务名；不写就当成给默认任务（图形界面）的参数
    task = DEFAULT_TASK
    if argv and not argv[0].startswith('-'):
        task = argv.pop(0)
    if task not in TASKS:
        try:
            sys.stderr.write('不认识的任务：%s\n\n%s' % (task, USAGE))
        except Exception:
            pass
        return 2

    try:
        plat, code = resolve_platform(requested)
        if code is not None:
            return code
        return run(plat, task, argv)
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    raise SystemExit(main())
