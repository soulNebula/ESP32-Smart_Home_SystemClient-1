# .idf-sandbox —— 自带 Windows + Linux 两套环境的绿色沙箱

这个目录就是"一台装好环境的虚拟机"，只不过它是一堆普通文件夹。

**它存在的意义**：客户电脑上什么都不用装 —— 不用装 C 编译器、CMake、Ninja、
Python、ESP-IDF，敲一条命令就能编译、烧录、看串口。

**Windows 和 Linux 两套工具链装在同一个沙箱里**，用哪套由当前系统自动决定
（两边都装了的话第一次会问一句）：

```bat
:: Windows
python .idf-sandbox\start.py          :: 或者双击 启动-Windows.cmd
```

```bash
# Linux
python3 .idf-sandbox/start.py         # 或者 sh .idf-sandbox/start-linux.sh
```

工具链是可执行文件，Windows 的是 PE（`.exe`）、Linux 的是 ELF，**不能互相运行**，
所以是"两套并排"而不是"一套通用"。选错会被提前拦住并说清楚原因，
不会甩出一堆 `Exec format error`。

## 环境从哪来：两种方式

**A. 拷文件夹（离线）** —— `.idf-sandbox/` 里环境是现成的，插上就能用，不联网。

**B. 在线配置** —— 如果这个目录里只有脚本（从 GitHub clone 下来就是这样），
点图形控制台里的 **「配置沙箱环境」**，或者跑：

```bat
python .idf-sandbox\start.py prepare --download     :: Windows
```
```bash
python3 .idf-sandbox/start.py prepare --download    # Linux
```

配套的两个命令（一般用图形界面上的按钮就行）：

```bat
python .idf-sandbox\start.py prepare --check          :: 自检：每项都真跑一遍
python .idf-sandbox\start.py prepare --prune          :: 看能清理多少（不删）
python .idf-sandbox\start.py prepare --prune --yes    :: 真删，腾硬盘
python .idf-sandbox\start.py prepare --fetch linux    :: 在 Windows 上给 Linux 备好工具链
```

它会从官方源把缺的东西下齐，**全部落在 `.idf-sandbox/` 里面**：

| 下什么 | 从哪下 | 多大 | 下完留多少 |
|---|---|---|---|
| ESP-IDF 5.4.4 源码 | `dl.espressif.com` | 1.9 GB | 357 MB（丢掉 docs/examples） |
| xtensa 交叉编译器 | `dl.espressif.com` | 250 MB | 1.4 GB |
| CMake / Ninja / ROM 链接脚本 | `dl.espressif.com` | 55 MB | 120 MB |
| ESP-IDF 的 Python 依赖 | PyPI | 60 MB | 约 200 MB |
| esp-sr 语音组件 | `components.espressif.com` | 230 MB | 230 MB |

不装 C 编译器、不改 PATH、不写注册表、不装全局 Python 包。
不想要了，删掉这个文件夹就干净了。

**下完自动自检**（每一项都真跑一遍，不是只看文件在不在）：

```
--- 自检 ------------------------------------------------------
  [OK]   ESP-IDF 源码        5.4.4
  [OK]   Python 环境         3.14.0
  [OK]   交叉编译器          xtensa-esp-elf-gcc-14.2.0 ...
  [OK]   idf.py 端到端       ESP-IDF v5.4.4
  [OK]   组件依赖            232.7 MB
  [OK]   自检全部通过 —— 可以点"一键编译烧录"了
```

单独体检：`prepare --check`。中途断了不要紧，再跑一次会接着下
（已下好的跳过，压缩包解压完立刻删）。

> Linux 上不打包 Python —— 各发行版都自带 `python3`，直接借它建虚拟环境。

## 里面有什么

```
.idf-sandbox/
├── start.py         ★ 总入口：认系统 → 选工具链 → 干活（逻辑全在里面）
├── 启动-Windows.cmd   Windows：双击这个
├── start-linux.sh     Linux：sh .idf-sandbox/start-linux.sh
├── 一键编译烧录.py    双击版 → start.py run
├── 只编译.py          双击版 → start.py build
├── 只看串口.py        双击版 → start.py monitor
├── 环境体检.py        双击版 → start.py doctor
├── 修复沙箱.py        双击版 → start.py prepare
│
├── tools/          入口背后真正干活的地方（一般不用看）
│   ├── core.py       沙箱定位 + 平台判定 + 环境变量
│   ├── gui.py        图形控制台（tkinter）
│   ├── run.py        一键：编译 + 烧录 + 串口监视器
│   ├── build.py      编译
│   ├── flash.py      烧录
│   ├── monitor.py    串口监视器
│   ├── doctor.py     环境体检
│   └── prepare.py    沙箱准备 / 修复
│
├── idf/            ESP-IDF 5.4.4 源码（纯 Python + C，两个平台共用）
│
├── python/         ┐
├── penv/           │ Windows 那一套
├── idf_tools/      │   ├── xtensa-esp-elf/  xtensa gcc 14.2.0（*.exe）
│                   │   ├── cmake/           CMake 3.30.2
│                   │   ├── ninja/           Ninja 1.12.1
│                   │   ├── esp-rom-elfs/    ROM 链接脚本
│                   │   └── idf-exe/         idf.py 包装器
├── drivers/        ┘ USB 转串口驱动（CH340 / CP210x / FTDI / USB-JTAG）
│
├── penv-linux/     ┐ Linux 那一套（Python 用系统自带的 python3）
└── idf_tools-linux/┘   同样四个工具，ELF 版
│
├── logs/           运行日志（编译 / 烧录 / 串口 / 体检）
├── tmp/            编译时的临时目录（防止系统 %TEMP% 不让写）
└── sandbox.json    版本清单
```

体积：Windows 侧约 2.2 GB（工具链 1.5 GB + ESP-IDF 源码 357 MB + Python 84 MB）。
Linux 侧那套（约 1.4 GB）默认不装 —— 要用先跑 `prepare --fetch linux`。


