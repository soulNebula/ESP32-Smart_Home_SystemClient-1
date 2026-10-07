#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""

  电脑上的蓝牙探针。手机 App 没装或者出问题时，用它当参照物，
  判断毛病在板子还是在 App。脚本用电脑蓝牙直接连板子，
  把服务、收发格式、推送、包长全验一遍，做的事和手机 App 一样：
  找到板子、连上、核对服务、订阅推送、发一条要状态、打印收到的内容。
  命令行用 scan / test / send / config / listen 几个子命令。
  要用 pip install bleak 装库，控制台中文已强制按 UTF-8 输出。
  扫设备时名字有时收不到，只认名字会把板子漏掉，所以服务广播里的服务号也认，
  靠它兜底才不会漏。
  发命令时按帧长选写类型：大帧要回执，小帧图快来去匆匆，免得多跑一趟。
  写数据老是报"操作被取消"，但只订阅不写数据时连接好得很，后来查出来是
  板子自己重启了：收命令这条链子全长在蓝牙主机任务里，一路走下来峰值用掉
  4~5KB 栈，而任务栈默认只有 4096，撑爆就重启，连接跟着断，Windows 就报被取消。
  固件已把栈加到 8192 并加了自检。所以看到连接莫名断开，先怀疑板子重启，
  别怪写类型选错了。


  扫板子
  连上核对服务
  发命令收数据
  只监听推送
"""

import argparse
import asyncio
import sys
import time

# 让中文不乱码
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("[FAIL] 没装 bleak。请先执行：pip install bleak")
    sys.exit(2)

# 收发用的固定编号
SERVICE_UUID = "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d"
# 手机 → 板子
RX_CHAR_UUID = "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5e"
# 板子 → 手机
TX_CHAR_UUID = "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5f"
NAME_PREFIX = "SmartHome-"

# 每帧开头写类型码
DOWN_CMD, DOWN_CONFIG, DOWN_GET = 0x01, 0x02, 0x03
UP_NAMES = {0x01: "state", 0x02: "sensor", 0x03: "ack", 0x04: "event", 0x05: "config"}

# 一包装不下，要放大包长
MTU_MIN_NEEDED = 256


def make_frame(type_code: int, payload: str = "") -> bytes:
    """类型码加内容拼一帧"""
    return bytes([type_code]) + payload.encode("utf-8")


def split_frame(data: bytes):
    """跳过帧头拿出内容"""
    if not data:
        return None, ""
    return data[0], data[1:].decode("utf-8", errors="replace")


def pretty(type_code: int, text: str) -> str:
    name = UP_NAMES.get(type_code, f"未知(0x{type_code:02X})")
    try:
        obj = __import__("json").loads(text)
        text = __import__("json").dumps(obj, ensure_ascii=False)
    except Exception:
        pass
    return f"[{name}] {text}"


def section(title: str):
    print()
    print("=" * 68)
    print(f" {title}")
    print("=" * 68)


# 找板子
async def do_scan(timeout: float, want_all: bool) -> list:
    section(f"扫描 BLE 设备（{timeout:.0f} 秒）...")
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)

    targets = []
    for device, adv in found.values():
        name = adv.local_name or device.name or ""

        # 名字和服务号都能认
        svc_uuids = [str(u).lower() for u in (adv.service_uuids or [])]
        by_name = name.startswith(NAME_PREFIX)
        by_uuid = SERVICE_UUID.lower() in svc_uuids
        hit = by_name or by_uuid

        if hit or want_all:
            mark = "★" if hit else " "
            how = "名字" if by_name else ("UUID" if by_uuid else "")
            suffix = f"   ← 命中({how})" if hit else ""
            print(f" {mark} {name or '(名字在扫描响应里，尚未收到)'}")
            print(f"     address = {device.address}   RSSI = {adv.rssi} dBm{suffix}")
            if svc_uuids:
                print(f"     service_uuids = {', '.join(svc_uuids)}")
        if hit:
            targets.append(device)

    print()
    if targets:
        print(f"找到 {len(targets)} 个 {NAME_PREFIX}* 设备")
    else:
        print(f"[FAIL] 没找到 {NAME_PREFIX}* 设备。排查：")
        print("       1) 固件是否启用了 BLE（menuconfig 里 '启用蓝牙 BLE 控制链路'）")
        print("       2) 开发板是否已上电、开机日志里有没有 BLE 启动成功")
        print("       3) 板子的天线是否插好（带天线座的板子不插天线信号会差很多）")
        print("       4) 换 --all 再扫一遍看是不是名字不对")
    return targets


async def pick_device(args):
    """按名字或地址挑板子"""
    if args.address:
        print(f"按地址直连：{args.address}")
        return args.address

    targets = await do_scan(args.timeout, want_all=False)
    if not targets:
        return None

    if args.name:
        for d in targets:
            if (d.name or "") == args.name:
                print(f"按名称匹配到：{d.name}  ({d.address})")
                return d
        print(f"[FAIL] 扫到的设备里没有名为 {args.name} 的")
        return None

    if len(targets) > 1:
        print(f"[WARN] 扫到多个 {NAME_PREFIX}* 设备，用第一个：{targets[0].name}")
        print("       要指定请加 --name 或 --address")
    return targets[0]


# 连上并自检
class Probe:
    def __init__(self, verbose: bool = True):
        self.client = None
        self.verbose = verbose
        # 收到的 (类型码, 文本, 时间戳)
        self.frames = []
        self.got = asyncio.Event()

    def on_notify(self, _sender, data: bytearray):
        tc, text = split_frame(bytes(data))
        self.frames.append((tc, text, time.time()))
        self.got.set()
        if self.verbose:
            print(f"  ← {pretty(tc, text)}")

    async def connect(self, device):
        section("连接并检查 GATT")
        self.client = BleakClient(device, timeout=20.0)
        await self.client.connect()
        print(f"  已连接: {device if isinstance(device, str) else device.address}")

        # Windows 自己谈包长
        mtu = getattr(self.client, "mtu_size", None)
        print(f"  协商 MTU = {mtu}")
        if isinstance(mtu, int):
            if mtu < MTU_MIN_NEEDED:
                print(f"  [WARN] MTU {mtu} < {MTU_MIN_NEEDED}，state JSON（约 250 字节）")
                print("         可能被截断。Windows 上 MTU 由系统协商，通常够用。")
            else:
                print(f"  [OK]   MTU 足够（可用载荷约 {mtu - 3} 字节）")

        # 数一遍服务，对编号
        section("GATT 服务核对")
        want_svc = SERVICE_UUID.lower()
        want_rx = RX_CHAR_UUID.lower()
        want_tx = TX_CHAR_UUID.lower()
        have_svc = have_rx = have_tx = False

        for svc in self.client.services:
            if svc.uuid.lower() == want_svc:
                have_svc = True
                print(f"  [OK]   Service  {svc.uuid}")
                for ch in svc.characteristics:
                    u = ch.uuid.lower()
                    if u == want_rx:
                        have_rx = True
                        print(f"    [OK]   RX  {ch.uuid}  props={ch.properties}")
                    elif u == want_tx:
                        have_tx = True
                        print(f"    [OK]   TX  {ch.uuid}  props={ch.properties}")
                    else:
                        print(f"          其他特征 {ch.uuid} props={ch.properties}")
            else:
                print(f"        其它服务 {svc.uuid}")

        if not have_svc:
            print(f"  [FAIL] 没找到 Service {SERVICE_UUID}")
            print("         固件的 UUID 写错了？注意 NimBLE 的 UUID 是小端序！")
            return False
        if not have_tx:
            print(f"  [FAIL] 没找到 TX 特征 {TX_CHAR_UUID}（收通知用）")
            return False
        if not have_rx:
            print(f"  [WARN] 没找到 RX 特征 {RX_CHAR_UUID}（发命令用）")

        # 订上，板子推消息
        section("订阅 TX 通知")
        await self.client.start_notify(TX_CHAR_UUID, self.on_notify)
        print("  [OK]   已订阅。接下来板子推的消息会实时打印：")
        return True

    async def send(self, type_code: int, payload: str = ""):
        frame = make_frame(type_code, payload)
        kind = {DOWN_CMD: "cmd", DOWN_CONFIG: "config", DOWN_GET: "get"}.get(type_code, "?")
        print(f"  → 发送 [{kind}] 帧长 {len(frame)} 字节：{payload or '(空)'}")

    # 发一条命令
        use_response = len(frame) > 20
        await self.client.write_gatt_char(RX_CHAR_UUID, frame, response=use_response)

    async def wait_frames(self, seconds: float, expect_at_least: int = 1):
        deadline = time.time() + seconds
        while time.time() < deadline and len(self.frames) < expect_at_least:
            self.got.clear()
            try:
                await asyncio.wait_for(self.got.wait(), timeout=max(0.1, deadline - time.time()))
            except asyncio.TimeoutError:
                break

    async def disconnect(self):
        if self.client and self.client.is_connected:
            try:
                await self.client.stop_notify(TX_CHAR_UUID)
            except Exception:
                pass
            await self.client.disconnect()
            print("  已断开")


# 几条子命令
async def cmd_test(args):
    device = await pick_device(args)
    if device is None:
        return 1

    p = Probe(verbose=True)
    try:
        if not await p.connect(device):
            return 1

        section("请求状态（发一帧 0x03 get）")
        await p.send(DOWN_GET, '{"get":"state"}')
        await p.wait_frames(max(args.wait, 5.0), expect_at_least=3)

        section("结果")
        kinds = {}
        for tc, _t, _ts in p.frames:
            kinds[UP_NAMES.get(tc, hex(tc))] = kinds.get(UP_NAMES.get(tc, hex(tc)), 0) + 1

        ok = True
        for need in ("state", "sensor", "config"):
            n = kinds.get(need, 0)
            if n:
                print(f"  [OK]   收到 {need} × {n}")
            else:
                print(f"  [FAIL] 没收到 {need}")
                ok = False

        print()
        if ok:
            print("  ✅ BLE 链路自检通过：GATT / 通知 / 帧格式 / JSON 全部正常")
        else:
            print("  ⚠️ 部分消息没收到。若 state 缺失，优先怀疑 MTU 太小导致截断。")
        return 0 if ok else 1
    finally:
        await p.disconnect()


async def cmd_send(args):
    device = await pick_device(args)
    if device is None:
        return 1
    p = Probe(verbose=True)
    try:
        if not await p.connect(device):
            return 1
        section("下发控制命令")
        await p.send(DOWN_CMD, args.json)
        await p.wait_frames(max(args.wait, 4.0), expect_at_least=2)
        section("收到 ack / state 即为成功")
        return 0
    finally:
        await p.disconnect()


async def cmd_config(args):
    device = await pick_device(args)
    if device is None:
        return 1
    p = Probe(verbose=True)
    try:
        if not await p.connect(device):
            return 1
        section("下发阈值配置")
        await p.send(DOWN_CONFIG, args.json)
        await p.wait_frames(max(args.wait, 4.0), expect_at_least=2)
        return 0
    finally:
        await p.disconnect()


async def cmd_listen(args):
    device = await pick_device(args)
    if device is None:
        return 1
    p = Probe(verbose=True)
    try:
        if not await p.connect(device):
            return 1
        section(f"持续监听 {args.seconds:.0f} 秒（Ctrl+C 可提前结束）")
        try:
            await asyncio.sleep(args.seconds)
        except asyncio.CancelledError:
            pass
        print(f"\n  共收到 {len(p.frames)} 帧")
        return 0
    finally:
        await p.disconnect()


def build_parser():
    ap = argparse.ArgumentParser(
        description="PC 端 BLE 探针：验证 ESP32-S3 智能家居的 BLE GATT 服务",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--timeout", type=float, default=8.0, help="扫描时长（秒，默认 8）")
    ap.add_argument("--name", default=None, help="指定设备名（默认自动取第一个 SmartHome-*）")
    ap.add_argument("--address", default=None, help="指定设备 MAC（跳过扫描直连）")
    ap.add_argument("--wait", type=float, default=5.0, help="发命令后等待回包的秒数")

    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("scan", help="只扫描，看能不能发现板子")
    s.add_argument("--all", action="store_true", help="列出所有设备，不只 SmartHome-*")

    sub.add_parser("test", help="完整自检：连接+订阅+请求状态+核对结果（最常用）")

    s = sub.add_parser("send", help="下发控制命令")
    s.add_argument("json", help='如 \'{"dev":"led_living","action":"on"}\'')

    s = sub.add_parser("config", help="下发阈值配置")
    s.add_argument("json", help='如 \'{"temp_fan_on_c":30}\'')

    s = sub.add_parser("listen", help="只监听通知")
    s.add_argument("--seconds", type=float, default=30.0, help="监听时长（默认 30 秒）")

    return ap


async def main_async(args):
    if args.cmd == "scan":
        targets = await do_scan(args.timeout, args.all)
        return 0 if targets else 1
    if args.cmd == "test":
        return await cmd_test(args)
    if args.cmd == "send":
        return await cmd_send(args)
    if args.cmd == "config":
        return await cmd_config(args)
    if args.cmd == "listen":
        return await cmd_listen(args)
    return 2


def main():
    args = build_parser().parse_args()
    try:
        code = asyncio.run(main_async(args))
    except KeyboardInterrupt:
        print("\n已中断")
        code = 130
    except Exception as e:
        print(f"\n[FAIL] {type(e).__name__}: {e}")
        code = 1
    sys.exit(code)


if __name__ == "__main__":
    main()
