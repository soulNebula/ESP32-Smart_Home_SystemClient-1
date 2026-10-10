# -*- coding: utf-8 -*-
"""ESP32-S3 智能家居 —— 可视化控制台

用 tkinter 写的一个小窗口，把命令行那套（编译 / 烧录 / 串口监视 / 体检）包成按钮：

    ┌──────────────────────────────────────────────────────┐
    │ 一键编译烧录  只编译  烧录  串口监视  停止            │
    │ 环境体检  修复沙箱  WiFi 配置  清空  打开日志         │
    │ 串口 [COM31 ▼] 刷新   波特率 [115200 ▼]  □先整片擦除  │
    ├──────────────────────────────────────────────────────┤
    │ （实时输出，带颜色）                                  │
    ├──────────────────────────────────────────────────────┤
    │ 发给板子: [__________________________] [发送]         │
    ├──────────────────────────────────────────────────────┤
    │ 状态…                              [进度条]           │
    └──────────────────────────────────────────────────────┘

为什么要用 tkinter：它是 Python 自带的，不用 pip 装任何东西。
沙箱里那个便携 Python 原本没带 tkinter，工程里已经把它补进 `.idf-sandbox/python`
了（Tcl/Tk 8.6，约 7 MB），所以客户机什么都不用装也能开这个窗口。

兼容 Python 3.9 ~ 3.14。
"""

from __future__ import annotations

import os
import queue
import sys
import threading
import time
import traceback
from pathlib import Path

# ---------------------------------------------------------------------------
# 起步：先把自己站住，再去 import 别的
# ---------------------------------------------------------------------------

if sys.version_info < (3, 9):
    raise SystemExit('[X] 需要 Python 3.9 或更高版本，当前是 ' + sys.version.split()[0])

# 本文件在 .idf-sandbox/tools/ 下：上一级是沙箱，再上一级是工程
SANDBOX = Path(__file__).resolve().parents[1]
LOG_DIR = SANDBOX / 'logs'
try:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
except OSError:
    pass


def _fix_stdio() -> None:
    """pythonw.exe 启动时没有控制台，sys.stdout/stderr 是 None，print 会炸

    这里给它们接上替代品：stdout 丢进黑洞，stderr 写进日志，出问题还能查。
    """
    if sys.stdout is None:
        sys.stdout = open(os.devnull, 'w', encoding='utf-8', errors='replace')
    if sys.stderr is None:
        try:
            sys.stderr = open(LOG_DIR / 'gui-error.log', 'a', encoding='utf-8', errors='replace')
        except OSError:
            sys.stderr = open(os.devnull, 'w', encoding='utf-8', errors='replace')


_fix_stdio()

sys.path.insert(0, str(Path(__file__).resolve().parent))

from core import (Reporter, Sandbox, desktop_dir, host_platform,  # noqa: E402
                  human_size, init_console, list_serial_ports, load_state,
                  open_path, run_streamed, save_state)


def show_error_box(title: str, message: str) -> None:
    """没有控制台的时候，至少弹个框让人看见"""
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, message, title, 0x10)
    except Exception:
        pass
    try:
        print(f'[{title}] {message}', file=sys.stderr)
    except Exception:
        pass


try:
    import tkinter as tk
    from tkinter import font as tkfont
    from tkinter import messagebox
    from tkinter import ttk
except Exception as _exc:                                     # pragma: no cover
    show_error_box('缺少 tkinter', (
        '这个 Python 没有 tkinter，图形界面起不来。\n\n'
        f'当前解释器：{sys.executable}\n'
        f'错误：{_exc}\n\n'
        'Windows 上请用沙箱自带的解释器启动（在工程根目录敲）：\n'
        r'  python .idf-sandbox\start.py' + '\n\n'
        'Linux 上 tkinter 是单独一个包，装一下就好：\n'
        '  sudo apt install python3-tk      （Debian/Ubuntu）\n'
        '  sudo dnf install python3-tkinter （Fedora）\n\n'
        '装完还是不行的话，用命令行版一样能干活：\n'
        '  python3 .idf-sandbox/start.py run'))
    raise SystemExit(2)


# ---------------------------------------------------------------------------
# 颜色
# ---------------------------------------------------------------------------

TAG_COLORS = {
    'red': '#c62828',
    'green': '#2e7d32',
    'yellow': '#b26a00',
    'cyan': '#00695c',
    'gray': '#757575',
    'dim': '#9e9e9e',
    'blue': '#1565c0',
    'bold': '#111111',
    'title': '#0d47a1',
    'sent': '#6a1b9a',
}

BAUD_LIST = ('115200', '230400', '460800', '921600', '57600', '38400', '19200', '9600')


def enable_dpi_awareness() -> float:
    """高分屏上别让窗口糊掉；顺便返回缩放比例，用来放大字体"""
    if os.name != 'nt':
        return 1.0
    try:
        import ctypes
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)      # Win 8.1+
        except Exception:
            ctypes.windll.user32.SetProcessDPIAware()           # 老系统
        hdc = ctypes.windll.user32.GetDC(0)
        dpi = ctypes.windll.gdi32.GetDeviceCaps(hdc, 88)        # LOGPIXELSX
        ctypes.windll.user32.ReleaseDC(0, hdc)
        return max(1.0, min(3.0, dpi / 96.0))
    except Exception:
        return 1.0


# ---------------------------------------------------------------------------
# 把 Reporter 的输出接到窗口里
# ---------------------------------------------------------------------------

class GuiReporter(Reporter):
    """和命令行那套完全一样的接口，只是把输出塞进队列，由主线程画到窗口上"""

    def __init__(self, out_queue: queue.Queue, log_file: Path | None = None):
        super().__init__(log_file=log_file, color=False)
        self.q = out_queue
        self._style: tuple = ()

    def c(self, text: str, *styles: str) -> str:
        # 窗口里不用 ANSI，把样式记下来，交给 line() 转成 tag
        self._style = tuple(s for s in styles if s in TAG_COLORS)
        return text

    def line(self, text: str = '') -> None:
        self.q.put(('line', text, self._style))
        self._style = ()
        self._log(text)

    def raw(self, text: str) -> None:
        for piece in text.splitlines() or ['']:
            self.q.put(('line', piece, ()))
        self._log(text, newline=False)

    def raw_replace(self, text: str) -> None:
        self.q.put(('replace', text, ()))
        self._log(text)


# ---------------------------------------------------------------------------
# 主窗口
# ---------------------------------------------------------------------------

class App:
    def __init__(self, root: tk.Tk, sandbox: Sandbox, scale: float = 1.0):
        self.root = root
        self.sandbox = sandbox
        # 沙箱（工具链）住在哪儿是固定的；被构建的工程可以随时换
        self.sandbox_root = sandbox.root
        self.home_project = sandbox.home
        self.scale = scale
        self.queue: queue.Queue = queue.Queue()
        self.reporter = GuiReporter(self.queue)
        self.busy = False
        self.monitor = None          # 正在跑的 monitor.Monitor
        self.ports = []
        self._progress_active = False
        self._closing = False

        state = load_state(sandbox)
        self.project_var = tk.StringVar(value=str(sandbox.project))
        self.recent_projects = [p for p in state.get('recent_projects', [])
                                if isinstance(p, str)]

        self._build_fonts()
        self._build_ui()
        self.refresh_ports()

        self.root.protocol('WM_DELETE_WINDOW', self.on_close)
        self.root.after(60, self._drain)

    # -- 界面搭起来 ---------------------------------------------------------

    def _build_fonts(self) -> None:
        s = self.scale
        try:
            families = set(tkfont.families())
        except tk.TclError:
            families = set()
        ui = 'Microsoft YaHei UI' if 'Microsoft YaHei UI' in families else 'Segoe UI'
        mono = 'Consolas' if 'Consolas' in families else 'Courier New'
        self.f_ui = (ui, max(9, int(9 * s)))
        self.f_ui_bold = (ui, max(9, int(9 * s)), 'bold')
        self.f_title = (ui, max(13, int(14 * s)), 'bold')
        self.f_mono = (mono, max(9, int(9 * s)))

    def _build_ui(self) -> None:
        self.root.title('ESP32-S3 智能家居 —— 控制台')

        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        self.root.minsize(int(640 * self.scale), int(400 * self.scale))

        style = ttk.Style()
        try:
            if 'vista' in style.theme_names():
                style.theme_use('vista')
        except tk.TclError:
            pass
        style.configure('Big.TButton', font=self.f_ui_bold, padding=(int(10 * self.scale), 5))
        # 「配置沙箱环境」是沙箱空着时的头等大事，给它一点存在感
        style.configure('Setup.TButton', font=self.f_ui_bold, padding=(int(7 * self.scale), 3))
        style.configure('TButton', font=self.f_ui, padding=(int(6 * self.scale), 3))
        style.configure('TLabel', font=self.f_ui)
        style.configure('TCheckbutton', font=self.f_ui)
        style.configure('TComboBox', font=self.f_ui)

        pad = int(8 * self.scale)
        outer = ttk.Frame(self.root, padding=pad)
        outer.pack(fill='both', expand=True)

        # ---- 底部两行先占位：这样窗口再小也不会把它们挤没 ------------------
        row5 = ttk.Frame(outer)
        row5.pack(side='bottom', fill='x', pady=(pad, 0))
        self.var_status = tk.StringVar(value='就绪')
        ttk.Label(row5, textvariable=self.var_status, foreground='#333333').pack(side='left')
        self.progress = ttk.Progressbar(row5, mode='indeterminate', length=int(150 * self.scale))
        self.progress.pack(side='right')

        row4 = ttk.Frame(outer)
        row4.pack(side='bottom', fill='x', pady=(pad, 0))
        ttk.Label(row4, text='发给板子').pack(side='left')
        self.var_send = tk.StringVar()
        self.ent_send = ttk.Entry(row4, textvariable=self.var_send, font=self.f_mono)
        self.ent_send.pack(side='left', fill='x', expand=True, padx=(6, 6))
        self.ent_send.bind('<Return>', lambda _e: self.send_line())
        self.btn_send = ttk.Button(row4, text='发送', width=7, command=self.send_line,
                                   state='disabled')
        self.btn_send.pack(side='left')

        # ---- 标题 ----
        head = ttk.Frame(outer)
        head.pack(side='top', fill='x')
        ttk.Label(head, text='ESP32 控制台', font=self.f_title).pack(side='left')
        self.lbl_env = ttk.Label(head, text='', foreground='#555555')
        self.lbl_env.pack(side='right')

        # ---- 要构建哪个工程 ----
        prow = ttk.Frame(outer)
        prow.pack(side='top', fill='x', pady=(pad, 0))
        ttk.Label(prow, text='工程').pack(side='left')
        self.cmb_project = ttk.Combobox(prow, textvariable=self.project_var,
                                        values=self.recent_projects, font=self.f_ui)
        self.cmb_project.pack(side='left', fill='x', expand=True, padx=(6, 4))
        self.cmb_project.bind('<Return>', lambda _e: self.apply_project())
        self.cmb_project.bind('<<ComboboxSelected>>', lambda _e: self.apply_project())
        ttk.Button(prow, text='浏览…', width=7, command=self.browse_project).pack(side='left')
        ttk.Button(prow, text='应用', width=6, command=self.apply_project).pack(
            side='left', padx=(4, 0))

        # ---- 主按钮 ----
        row1 = ttk.Frame(outer)
        row1.pack(side='top', fill='x', pady=(pad, 0))
        self.btn_all = ttk.Button(row1, text='一键编译烧录', style='Big.TButton',
                                  command=self.task_all)
        self.btn_all.pack(side='left')
        self.btn_build = ttk.Button(row1, text='只编译', command=self.task_build)
        self.btn_build.pack(side='left', padx=(6, 0))
        self.btn_flash = ttk.Button(row1, text='烧录', command=self.task_flash)
        self.btn_flash.pack(side='left', padx=(6, 0))
        self.btn_mon = ttk.Button(row1, text='看串口', command=self.task_monitor)
        self.btn_mon.pack(side='left', padx=(6, 0))
        self.btn_stop = ttk.Button(row1, text='停止', command=self.stop_task, state='disabled')
        self.btn_stop.pack(side='left', padx=(6, 0))

        # ---- 串口选项 ----
        row3 = ttk.Frame(outer)
        row3.pack(side='top', fill='x', pady=(6, 0))
        ttk.Label(row3, text='串口').pack(side='left')
        self.cmb_port = ttk.Combobox(row3, width=int(26 * self.scale), state='readonly',
                                     font=self.f_ui)
        self.cmb_port.pack(side='left', padx=(6, 4))
        ttk.Button(row3, text='刷新', width=5, command=self.refresh_ports).pack(side='left')
        ttk.Label(row3, text='波特率').pack(side='left', padx=(int(10 * self.scale), 0))
        self.cmb_baud = ttk.Combobox(row3, width=8, state='readonly',
                                     values=list(BAUD_LIST), font=self.f_ui)
        self.cmb_baud.set('115200')
        self.cmb_baud.pack(side='left', padx=(6, 0))
        self.var_erase = tk.BooleanVar(value=False)
        ttk.Checkbutton(row3, text='先擦除', variable=self.var_erase).pack(
            side='left', padx=(int(10 * self.scale), 0))
        self.var_reset = tk.BooleanVar(value=True)
        ttk.Checkbutton(row3, text='连上复位', variable=self.var_reset).pack(
            side='left', padx=(int(6 * self.scale), 0))

        # ---- 工具按钮 ----
        row2 = ttk.Frame(outer)
        row2.pack(side='top', fill='x', pady=(6, 0))
        self.btn_setup = ttk.Button(row2, text='配置沙箱环境', style='Setup.TButton',
                                    command=self.task_setup)
        self.btn_setup.pack(side='left')
        ttk.Button(row2, text='环境体检', command=self.task_doctor).pack(side='left', padx=(6, 0))
        ttk.Button(row2, text='修复沙箱', command=self.task_repair).pack(side='left', padx=(6, 0))
        ttk.Button(row2, text='清理空间', command=self.task_prune).pack(side='left', padx=(6, 0))
        ttk.Button(row2, text='WiFi 配置', command=self.open_wifi_config).pack(
            side='left', padx=(6, 0))
        ttk.Button(row2, text='打开日志', command=self.open_logs).pack(side='right')
        ttk.Button(row2, text='清空输出', command=self.clear_log).pack(side='right', padx=(0, 6))

        # ---- 输出区（吃掉剩下的空间） ----
        logbox = ttk.Frame(outer)
        logbox.pack(side='top', fill='both', expand=True, pady=(pad, 0))
        self.log = tk.Text(logbox, wrap='word', font=self.f_mono, state='disabled',
                           height=12, background='#fbfbfb', foreground='#212121',
                           relief='solid', borderwidth=1, padx=8, pady=6,
                           insertbackground='#212121')
        scroll = ttk.Scrollbar(logbox, orient='vertical', command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)
        for tag, color in TAG_COLORS.items():
            self.log.tag_configure(tag, foreground=color)

        self._update_env_label()
        self.append('提示：改完代码点「一键编译烧录」就行；'
                    '板子没插也能点「只编译」检查语法。', 'dim')
        self.append('', '')

        # 让窗口刚好装下内容：按"内容需要多大"来定，而不是硬猜一个尺寸。
        # 屏幕实在太小才压缩，这时会被压扁的是下面的日志框（它带 expand），
        # 底部那两行是先 pack 的，压不到。
        self.root.update_idletasks()
        want_w = self.root.winfo_reqwidth()
        want_h = self.root.winfo_reqheight()
        width = min(want_w, screen_w - int(30 * self.scale))
        height = min(want_h, screen_h - int(70 * self.scale))
        self.root.geometry(f'{max(width, 1)}x{max(height, 1)}')
        self._centred = False

    def _update_env_label(self) -> None:
        sb = self.sandbox
        name = sb.project_name()
        mark = '（外部工程）' if sb.is_foreign else ''
        plat = {'windows': 'Windows', 'linux': 'Linux', 'macos': 'macOS'}.get(sb.host, sb.host)
        self.lbl_env.configure(
            text=f'{name}{mark} · {sb.chip_target} · ESP-IDF {sb.idf_version or "?"} · '
                 f'{plat} · Python {sys.version.split()[0]}')
        self.root.title(f'ESP32 控制台 —— {name}')
        # 沙箱体积要遍历 5 万多个文件，几秒钟起步 —— 扔到后台算，别卡住开窗
        threading.Thread(target=self._measure_sandbox, daemon=True).start()

    # -- 换工程 ---------------------------------------------------------------

    def apply_project(self) -> None:
        """把输入框里的路径变成"当前要构建的工程"

        沙箱（工具链、Python）不动，只换要编译的那份源码。
        """
        raw = self.project_var.get().strip().strip('"')
        if not raw:
            self.reset_project()
            return
        path = Path(raw)
        if not path.is_dir():
            self.append(f'目录不存在：{path}', 'red')
            return
        if self.busy:
            self.append('有任务正在跑，等它结束再换工程。', 'yellow')
            self.project_var.set(str(self.sandbox.project))
            return
        try:
            path = path.resolve()
        except OSError:
            pass

        self.sandbox = Sandbox(project=path, sandbox_dir=self.sandbox_root)
        problems = self.sandbox.project_problems()
        if problems:
            self.append(f'[注意] {path} 看着不太像 ESP-IDF 工程：', 'yellow')
            for item in problems:
                self.append('       ' + item, 'yellow')
            for item in self.sandbox.project_hint():
                self.append('       ' + item if item.startswith(' ') else '   ' + item,
                            'dim')
        else:
            self.append('', '')
            self.append(f'已切换工程：{self.sandbox.project_name()}   {path}', 'cyan')
            if self.sandbox.is_foreign:
                self.append(f'  工具链仍然用沙箱里的：{self.sandbox_root}', 'dim')
            built = (path / 'build' / 'flasher_args.json').is_file()
            self.append('  这个工程' + ('已经编译过，可以直接点「烧录」'
                                        if built else '还没编译过，先点「一键编译烧录」'),
                        'dim')

        recent = [str(path)] + [p for p in self.recent_projects if p != str(path)]
        self.recent_projects = recent[:8]
        try:
            self.cmb_project.configure(values=self.recent_projects)
        except tk.TclError:
            pass
        self._update_env_label()

    def browse_project(self) -> None:
        """选工程目录：默认从桌面开始翻（工程一般都放桌面上）"""
        from tkinter import filedialog
        start = desktop_dir()
        if not start.is_dir():
            current = self.project_var.get().strip()
            start = Path(current) if current and Path(current).is_dir() else self.home_project
        chosen = filedialog.askdirectory(
            title='选择要编译的 ESP-IDF 工程目录（里面应该有 CMakeLists.txt）',
            initialdir=str(start))
        if chosen:
            self.project_var.set(chosen)
            self.apply_project()

    def reset_project(self) -> None:
        """回到沙箱自带那个工程（输入框被清空时会走这里）"""
        self.project_var.set(str(self.home_project))
        self.apply_project()

    def _measure_sandbox(self) -> None:
        try:
            total = 0
            for root, _dirs, files in os.walk(self.sandbox.root):
                for name in files:
                    try:
                        total += os.path.getsize(os.path.join(root, name))
                    except OSError:
                        pass
            self.queue.put(('env_size', human_size(total), ()))
        except Exception:
            pass

    # -- 输出区 -------------------------------------------------------------

    def _tag_for(self, text: str, style: tuple) -> str:
        if style:
            return style[0]
        stripped = text.strip()
        if stripped.startswith(('[OK]', '  [OK]')):
            return 'green'
        if stripped.startswith('[缺]') or stripped.startswith('[失败]'):
            return 'red'
        if stripped.startswith('[注意]'):
            return 'yellow'
        if stripped.startswith('E (') or ' E (' in text:
            return 'red'
        if stripped.startswith('W (') or ' W (' in text:
            return 'yellow'
        if stripped.startswith('D (') or stripped.startswith('V ('):
            return 'gray'
        if any(k in text for k in ('ESP-ROM', 'boot:', 'rst:0x')):
            return 'cyan'
        if any(k in text for k in ('got ip', 'connected', 'MQTT', 'system ready',
                                   'board init', 'ready')):
            return 'green'
        return ''

    def append(self, text: str, style: str = '') -> None:
        tag = style or self._tag_for(text, ())
        self.log.configure(state='normal')
        self.log.insert('end', text + '\n', tag)
        self.log.configure(state='disabled')
        self.log.see('end')
        self._progress_active = False

    def append_replace(self, text: str) -> None:
        """进度条那种原地刷新：把上一行盖掉"""
        self.log.configure(state='normal')
        if self._progress_active:
            self.log.delete('end-1c linestart', 'end')
        self.log.insert('end', text + '\n', self._tag_for(text, ()))
        self.log.configure(state='disabled')
        self.log.see('end')
        self._progress_active = True

    def clear_log(self) -> None:
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.configure(state='disabled')
        self._progress_active = False

    def _drain(self) -> None:
        if self._closing:
            return
        handled = 0
        try:
            while handled < 500:
                kind, text, style = self.queue.get_nowait()
                handled += 1
                if kind == 'line':
                    self.append(text, style[0] if style else '')
                elif kind == 'replace':
                    self.append_replace(text)
                elif kind == 'status':
                    self.var_status.set(text)
                elif kind == 'env_size':
                    self._set_env_size(text)
                elif kind == 'enable_send':
                    self.btn_send.configure(state='normal')
                    self.ent_send.focus_set()
                elif kind == 'disable_send':
                    self.btn_send.configure(state='disabled')
                elif kind == 'done':
                    self._task_finished(text, style)
        except queue.Empty:
            pass
        self.root.after(60, self._drain)

    # -- 任务调度 -----------------------------------------------------------

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        state = 'disabled' if busy else 'normal'
        for btn in (self.btn_all, self.btn_build, self.btn_flash, self.btn_mon):
            btn.configure(state=state)
        self.btn_stop.configure(state='normal' if busy else 'disabled')
        if busy:
            self.progress.start(14)
        else:
            self.progress.stop()

    def _start(self, name: str, func) -> None:
        if self.busy:
            self.append('还有任务在跑，先等它结束或者点「停止」。', 'yellow')
            return
        self._set_busy(True)
        self.var_status.set(name + ' ……')
        threading.Thread(target=self._worker, args=(name, func), daemon=True).start()

    def _worker(self, name: str, func) -> None:
        code = 1
        try:
            code = func()
        except SystemExit as exc:
            code = int(exc.code) if isinstance(exc.code, int) else (0 if exc.code is None else 1)
        except Exception:
            self.queue.put(('line', traceback.format_exc(), ('red',)))
        finally:
            self.queue.put(('done', name, (str(code),)))
            self.queue.put(('status', '', ()))

    def _task_finished(self, name: str, style: tuple) -> None:
        code = int(style[0]) if style and style[0].isdigit() else 1
        self._set_busy(False)
        if code == 0:
            self.var_status.set(f'{name} 完成')
            self.append('', '')
            self.append(f'—— {name} 完成 ——', 'green')
        else:
            self.var_status.set(f'{name} 失败（退出码 {code}）')
            self.append('', '')
            self.append(f'—— {name} 失败（退出码 {code}） ——', 'red')

    def stop_task(self) -> None:
        if self.monitor is not None:
            self.append('正在停止串口监视……', 'yellow')
            self.monitor.stop()
        else:
            self.append('编译/烧录正在跑，停不了；等它结束吧。', 'yellow')

    # -- 各项任务 -----------------------------------------------------------

    def _need_sandbox(self) -> bool:
        missing = self.sandbox.check_essentials()
        if missing:
            self.append('沙箱不完整，缺少：', 'red')
            for item in missing:
                self.append('    ' + item, 'red')
            self.append('点「修复沙箱」试试，或者把整个 .idf-sandbox 目录重新拷一遍。', 'dim')
            return False
        return True

    def _port(self) -> str:
        value = self.cmb_port.get()
        if not value:
            return ''
        return value.split()[0]

    def _baud(self) -> int:
        try:
            return int(self.cmb_baud.get())
        except ValueError:
            return 115200

    def task_build(self, clean: bool = False):
        if not self._need_sandbox():
            return

        def job() -> int:
            from build import run_build
            return run_build(self.sandbox, self.reporter, clean=clean)

        self._start('编译', job)

    def task_flash(self):
        if not self._need_sandbox():
            return
        port = self._port()
        if not port:
            self.append('先在上面选一个串口（没有的话点「刷新」，还不行就是驱动没装）。', 'yellow')
            return

        def job() -> int:
            from flash import run_flash
            code, _ = run_flash(self.sandbox, self.reporter, port=port,
                                erase=self.var_erase.get())
            return code

        self._start('烧录', job)

    def task_monitor(self):
        port = self._port()
        if not port:
            self.append('先在上面选一个串口。', 'yellow')
            return
        self._start('串口监视', lambda: self._monitor_job(port))

    def _monitor_job(self, port: str) -> int:
        """连上串口一直读，直到点「停止」或者出错"""
        from monitor import Monitor
        self.sandbox.log_dir.mkdir(parents=True, exist_ok=True)
        log_path = self.sandbox.log_dir / f'monitor-{time.strftime("%Y%m%d-%H%M%S")}.log'
        mon = Monitor(self.reporter, port, self._baud(), self.var_reset.get(),
                      interactive=False, log_path=log_path, seconds=0.0)
        mon.announce = False
        try:
            if not mon.open():
                return 1
            self.monitor = mon
            self.queue.put(('status', f'正在监视 {port}，波特率 {self._baud()}', ()))
            self.queue.put(('line', f'已连上 {port} @ {self._baud()}；'
                                    f'下面可以直接发命令给板子。', ('cyan',)))
            self.queue.put(('enable_send', '', ()))
            try:
                mon.run()
            except KeyboardInterrupt:
                pass
            self.queue.put(('line', '串口监视已停止。', ('dim',)))
            self.queue.put(('line', f'串口记录：{log_path}', ('dim',)))
            return 0
        finally:
            self.monitor = None
            self.queue.put(('disable_send', '', ()))
            try:
                mon.close()
            except Exception:
                pass

    def task_all(self):
        """一键：编译 → 烧录 → 看串口"""
        if not self._need_sandbox():
            return
        port = self._port()
        if not port:
            self.append('没选串口。只想检查代码的话点「只编译」。', 'yellow')
            return

        def job() -> int:
            from build import run_build
            from flash import run_flash
            code = run_build(self.sandbox, self.reporter)
            if code != 0:
                self.reporter.error('编译没过，先不烧录了')
                return code
            code, used = run_flash(self.sandbox, self.reporter, port=port,
                                   erase=self.var_erase.get())
            if code != 0:
                self.reporter.error('烧录没成功，串口监视先不开')
                return code
            self.queue.put(('status', f'烧录完成，正在打开 {used}', ()))
            return self._monitor_job(used)

        self._start('一键编译烧录', job)

    def task_doctor(self):
        self._start('环境体检', lambda: self._run_script('doctor.py', ['--ports']))

    def task_repair(self):
        self._start('修复沙箱', lambda: self._run_script('prepare.py', ['--repair']))

    def task_setup(self):
        """在线配置沙箱环境：缺什么下什么，下完自检

        全部下载到 .idf-sandbox/ 里面，不往系统目录写东西。
        第一次要联网、要等一会儿（大约 2 GB），之后就一直离线可用了。
        """
        sandbox = self.sandbox
        size = self._dir_size(sandbox.root)
        missing = sandbox.check_essentials()
        lines = [
            '这个按钮会联网把沙箱配齐（缺什么下什么，已有的跳过）。',
            '',
            f'  当前沙箱：{human_size(size)}',
            f'  下载到  ：{sandbox.root}',
            '',
            '全部放在沙箱目录里，不装到系统、不改 PATH、不动注册表。',
            '第一次大约要下 2 GB（走 Espressif 国内镜像），之后完全离线。',
            '中途断了不要紧，再点一次会接着下。',
        ]
        if missing:
            lines.append('')
            lines.append('现在缺的东西：')
            for item in missing[:6]:
                lines.append(f'  · {item.split()[0]}')
        if not messagebox.askokcancel('配置沙箱环境', '\n'.join(lines)):
            return
        self._start('配置沙箱环境',
                    lambda: self._run_script('prepare.py', ['--download']))

    def task_prune(self):
        """清理空间：删掉编译产物和另一个平台的工具链

        删的全是"没了也能再弄回来"的东西，所以删完沙箱照样能用。
        """
        sandbox = self.sandbox
        other = 'linux' if sandbox.host == 'windows' else 'windows'
        targets = [
            ('编译产物 build/', sandbox.project / 'build'),
            ('下载缓存 download/', sandbox.root / 'download'),
            ('日志 logs/', sandbox.root / 'logs'),
            (f'另一个平台的工具链 idf_tools-{other}/', sandbox.root / f'idf_tools-{other}'),
        ]
        lines = ['下面这些会被删掉（都是能重新生成/重新下载的）：', '']
        freed = 0
        for label, path in targets:
            if path.exists():
                size = self._dir_size(path)
                freed += size
                lines.append(f'  · {label}   {human_size(size)}')
        if not freed:
            messagebox.showinfo('清理空间', '已经很干净了，没有可删的东西。')
            return
        lines += [
            '',
            f'合计可以释放 {human_size(freed)}。',
            '',
            '删除不影响使用：编译产物下次编译自动重建；',
            f'另一个平台的工具链要用时这样弄回来：',
            f'    prepare --fetch {other}',
        ]
        if not messagebox.askokcancel('清理空间', '\n'.join(lines)):
            return
        self._start('清理空间',
                    lambda: self._run_script('prepare.py', ['--prune', '--yes']))

    @staticmethod
    def _dir_size(path) -> int:
        total = 0
        try:
            for root, _dirs, files in os.walk(path):
                for name in files:
                    try:
                        total += os.path.getsize(os.path.join(root, name))
                    except OSError:
                        pass
        except OSError:
            pass
        return total

    def _run_script(self, script: str, extra: list) -> int:
        """体检、修复这类脚本直接当子进程跑，输出照样流进窗口"""
        path = Path(__file__).resolve().parent / script
        if not path.is_file():
            self.append(f'找不到脚本：{path}', 'red')
            return 2
        # 配置环境要用当前这个解释器（可能就是系统 Python），
        # 不能用沙箱那个 —— 沙箱 Python 可能正是缺的那一块
        base = self.sandbox.python_exe
        python = str(base) if base.is_file() else sys.executable
        cmd = [python, str(path), *extra, '--no-pause']
        env = self.sandbox.build_env()
        return run_streamed(cmd, env, self.sandbox.project, self.reporter).code

    # -- 杂项 ---------------------------------------------------------------

    def send_line(self) -> None:
        text = self.var_send.get().strip()
        if not text:
            return
        if self.monitor is None:
            self.append('串口还没连上，先点「看串口」。', 'yellow')
            return
        self.append('> ' + text, 'sent')
        self.monitor.send(text)
        self.var_send.set('')

    def _set_env_size(self, size: str) -> None:
        sb = self.sandbox
        name = sb.project_name()
        mark = '（外部工程）' if sb.is_foreign else ''
        self.lbl_env.configure(
            text=f'{name}{mark} · {sb.chip_target} · ESP-IDF {sb.idf_version or "?"} · '
                 f'Python {sys.version.split()[0]} · 沙箱 {size}')

    def refresh_ports(self) -> None:
        self.ports = list_serial_ports()
        values = []
        for info in self.ports:
            friendly = info.friendly
            if len(friendly) > 22:
                friendly = friendly[:21] + '…'
            note = f' · {info.note}' if info.note else ''
            values.append(f'{info.device}  {friendly}{note}')
        self.cmb_port.configure(values=values)

        saved = str(load_state(self.sandbox).get('port', ''))
        pick = ''
        for index, info in enumerate(self.ports):
            if saved and info.device == saved:
                pick = values[index]
                break
        if not pick and values:
            pick = values[0]
        self.cmb_port.set(pick)

        if not self.ports:
            self.append('没扫到串口。板子插好了吗？驱动装了吗'
                        '（见 .idf-sandbox\\drivers\\README.md）？', 'yellow')
        elif all(p.is_virtual for p in self.ports):
            self.append('扫到的都是虚拟串口（蓝牙之类），没看到开发板。', 'yellow')

    def open_logs(self) -> None:
        try:
            self.sandbox.log_dir.mkdir(parents=True, exist_ok=True)
            open_path(self.sandbox.log_dir)      # Windows 用 startfile，Linux 用 xdg-open
        except Exception as exc:
            self.append(f'打不开日志目录：{exc}', 'red')

    def open_wifi_config(self) -> None:
        path = self.sandbox.project / 'components' / 'App' / 'wifi_config.h'
        if not path.is_file():
            self.append(f'找不到 {path}', 'red')
            return
        try:
            open_path(path)
            self.append(f'已打开 {path}；改完记得回来点「一键编译烧录」。', 'cyan')
        except Exception as exc:
            self.append(f'打不开：{exc}', 'red')

    def on_close(self) -> None:
        if self.monitor is not None:
            self.monitor.stop()
        elif self.busy:
            from tkinter import messagebox
            if not messagebox.askokcancel('还在忙', '任务还没结束，确定要关掉吗？'):
                return
        try:
            save_state({'port': self._port(), 'baud': self._baud(),
                        'last_project': str(self.sandbox.project),
                        'recent_projects': self.recent_projects}, self.sandbox)
        except Exception:
            pass
        self._closing = True
        try:
            self.root.destroy()
        except tk.TclError:
            pass


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------

def main(argv: list | None = None) -> int:
    init_console()
    if argv is None:
        argv = sys.argv[1:]

    def arg_value(*keys: str) -> str:
        for key in keys:
            if key in argv:
                try:
                    return argv[argv.index(key) + 1]
                except IndexError:
                    return ''
        return ''

    if '-h' in argv or '--help' in argv:
        # 这个脚本没有 argparse（不然 --help 会变成开窗口），手写一份说明
        print('ESP32 控制台 —— 图形界面（tkinter，不用装任何库）\n')
        print('用法：')
        print('  gui.py [--project 工程目录] [-p 串口]\n')
        print('  --project   要编译的 ESP-IDF 工程目录；不填就接着上次用的那个')
        print('  -p, --port  启动时预选哪个串口，例如 -p COM31')
        print()
        print('窗口里可以随时换工程：顶上「工程」那一行，填路径或点「浏览…」再「应用」。')
        print('工具链（gcc / cmake / ninja / Python / ESP-IDF）始终用沙箱里的那一套。')
        return 0

    # 注意：-p/--port 是串口，--project 才是工程目录，全工程统一
    project = arg_value('--project')

    home = Sandbox()
    if not project:
        # 没在命令行指定，就接着上次用的那个工程（第一次用就是沙箱自己那个）
        remembered = str(load_state(home).get('last_project', ''))
        if remembered and Path(remembered).is_dir():
            project = remembered

    sandbox = Sandbox(project=project or None, sandbox_dir=home.root)
    scale = enable_dpi_awareness()

    root = tk.Tk()
    app = App(root, sandbox, scale)

    want_port = arg_value('-p', '--port')
    if want_port:
        for index, info in enumerate(app.ports):
            if info.device.lower() == want_port.lower():
                app.cmb_port.current(index)
                break

    try:
        root.mainloop()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    raise SystemExit(main())