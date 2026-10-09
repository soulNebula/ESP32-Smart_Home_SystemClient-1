# -*- coding: utf-8 -*-
"""后台盯着 github.com，一通就推上去

这台机器到 github.com 是间歇性不通（dl.espressif.com 一直很快，
所以不是断网）。提交已经在本地了，丢不了；这个脚本负责在路通的那一刻推上去。
"""
import socket
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

REPO = r'C:\Users\Administrator\Desktop\esp32_smart_home'
DEADLINE = time.time() + 45 * 60      # 最多盯 45 分钟
ATTEMPT = 0


def reachable(host='github.com', port=443, timeout=8):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def out(text):
    sys.stdout.write(text + '\n')
    sys.stdout.flush()


out('开始盯 github.com（最多 45 分钟）')
while time.time() < DEADLINE:
    ATTEMPT += 1
    waited = int(time.time() - (DEADLINE - 45 * 60))
    if not reachable():
        out('[%4ds] 第 %d 次：443 还不通' % (waited, ATTEMPT))
        time.sleep(45)
        continue

    out('[%4ds] 第 %d 次：通了！开始推送' % (waited, ATTEMPT))
    result = subprocess.run(['git', 'push', 'origin', 'main'], cwd=REPO,
                            capture_output=True, text=True,
                            encoding='utf-8', errors='replace')
    for line in ((result.stdout or '') + (result.stderr or '')).splitlines():
        out('        ' + line)
    if result.returncode == 0:
        out('')
        out('推送成功 ✓')
        # 确认远端 SHA 和本地一致
        check = subprocess.run(['git', 'ls-remote', '--heads', 'origin'],
                               cwd=REPO, capture_output=True, text=True,
                               encoding='utf-8', errors='replace')
        local = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=REPO,
                               capture_output=True, text=True,
                               encoding='utf-8', errors='replace')
        out('  远端: ' + (check.stdout or '').strip()[:60])
        out('  本地: ' + (local.stdout or '').strip()[:60])
        sys.exit(0)

    out('        推送失败，等 60 秒再来')
    time.sleep(60)

out('45 分钟到了还是没通。提交在本地，路通了手动 git push 即可。')
sys.exit(1)
