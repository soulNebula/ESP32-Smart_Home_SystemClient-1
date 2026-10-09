# -*- coding: utf-8 -*-
r"""编译 + 烧录 + 看串口（等价于 python start.py run）

双击本文件也行。真正的启动逻辑全在 start.py 里。"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import start

raise SystemExit(start.main(['run'] + sys.argv[1:]))