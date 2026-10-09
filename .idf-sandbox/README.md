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

体积约 3.3 GB（Windows 侧 1.9 GB + Linux 侧 1.4 GB），其中工具链占大头。


