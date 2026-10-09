# -*- coding: utf-8 -*-
r"""只看串口输出（等价于 python start.py monitor）

真正的启动逻辑全在 start.py 里。"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import start

raise SystemExit(start.main(['monitor'] + sys.argv[1:]))