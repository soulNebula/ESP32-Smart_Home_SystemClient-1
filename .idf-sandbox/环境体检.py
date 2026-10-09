# -*- coding: utf-8 -*-
r"""环境体检（等价于 python start.py doctor）

出问题了先跑这个，截图发出来最快定位。"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import start

raise SystemExit(start.main(['doctor'] + sys.argv[1:]))