# -*- coding: utf-8 -*-
"""上传 GitHub 前的体检：体积统计 + 敏感信息扫描"""
import re
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# git 输出的路径可能带引号和八进制转义，用 -z + core.quotepath=false 拿原始字节
raw = subprocess.run(['git', '-c', 'core.quotepath=false', 'ls-files', '-co',
                      '--exclude-standard', '-z'],
                     capture_output=True).stdout
paths = [p for p in raw.decode('utf-8', 'replace').split('\0') if p]

print('=== 会被提交的文件：%d 个 ===' % len(paths))

sizes = []
for p in paths:
    f = Path(p)
    if f.is_file():
        sizes.append((f.stat().st_size, p))

sizes.sort(reverse=True)
total = sum(s for s, _ in sizes)
print('总体积：%.1f MB' % (total / 1048576))
print()
print('--- 最大的 12 个 ---')
for s, p in sizes[:12]:
    print('  %8.2f MB  %s' % (s / 1048576, p))

print()
over100 = [(s, p) for s, p in sizes if s > 100 * 1048576]
over50 = [(s, p) for s, p in sizes if 50 * 1048576 < s <= 100 * 1048576]
print('超过 100 MB（GitHub 直接拒绝）:', len(over100))
for s, p in over100:
    print('   !! %.1f MB %s' % (s / 1048576, p))
print('超过 50 MB（会有警告）:', len(over50))
for s, p in over50:
    print('   ~  %.1f MB %s' % (s / 1048576, p))

# ---------------------------------------------------------------------------
print()
print('=== 敏感信息扫描 ===')
PATTERNS = [
    ('WiFi 真密码', re.compile(r'#define\s+APP_WIFI_(?:SSID|PASSWORD)\s+"(?!YOUR_)[^"]{2,}"')),
    ('API key/token', re.compile(r'(?i)\b(api[_-]?key|secret|token|passwd|password)\b\s*[:=]\s*["\']([^"\']{8,})["\']')),
    ('私钥文件头', re.compile(r'BEGIN (?:RSA |OPENSSH |EC |PGP )?PRIVATE KEY')),
    ('AWS key', re.compile(r'AKIA[0-9A-Z]{16}')),
    ('GitHub token', re.compile(r'gh[pousr]_[A-Za-z0-9]{36,}')),
    ('MQTT 账号密码', re.compile(r'(?i)mqtt.{0,20}(user|pass).{0,10}[:=]\s*["\'][^"\']{3,}["\']')),
]
SKIP_EXT = {'.png', '.jpg', '.jpeg', '.gif', '.ico', '.apk', '.jar', '.zip',
            '.stl', '.sldprt', '.pdf', '.bin', '.elf', '.a', '.o'}
SKIP_DIRS = ('.git/', 'components/u8g2/csrc/u8g2_fonts.c', 'components/u8g2/csrc/u8x8_fonts.c')

hits = 0
for _s, p in sizes:
    if p.endswith(tuple(SKIP_EXT)) or p.startswith(SKIP_DIRS) or p in SKIP_DIRS:
        continue
    try:
        text = Path(p).read_text(encoding='utf-8', errors='ignore')
    except OSError:
        continue
    for label, rx in PATTERNS:
        for m in rx.finditer(text):
            line_no = text[:m.start()].count('\n') + 1
            snippet = m.group(0).replace('\n', ' ')[:90]
            print('  [%s] %s:%d  %s' % (label, p, line_no, snippet))
            hits += 1
print()
print('敏感信息命中：%d 处' % hits)
if hits == 0:
    print('  （没有发现真实密钥/密码）')

# ---------------------------------------------------------------------------
print()
print('=== 不该提交的垃圾文件检查 ===')
JUNK = ['.d', '.o', '.obj', '.pyc', '.log', '.tmp', '.bak', '.orig']
junk = [p for _s, p in sizes if Path(p).suffix.lower() in JUNK]
for p in junk[:20]:
    print('  ?', p)
print('  可疑构建产物：%d 个' % len(junk))
