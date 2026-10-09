# -*- coding: utf-8 -*-
r"""修复沙箱（等价于 python start.py prepare）

缺什么补什么，真正的启动逻辑全在 start.py 里。"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import start

raise SystemExit(start.main(['prepare'] + sys.argv[1:]))