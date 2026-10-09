# -*- coding: utf-8 -*-
r"""只编译，不烧录（等价于 python start.py build）

板子没插也能跑。真正的启动逻辑全在 start.py 里。"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import start

raise SystemExit(start.main(['build'] + sys.argv[1:]))