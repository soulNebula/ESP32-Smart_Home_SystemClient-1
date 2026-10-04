#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
voice_debug.py —— ESP32 智能家居「语音识别」本地调试器（PC 版）

把固件里的 ESP-SR 唤醒链（INMP441 + WakeNet + MultiNet）用 vosk 离线中文
识别在 PC 上复刻一遍，方便在本地调试命令词和 JSON 事件，不用反复烧板子。

行为与固件一致（docs/13-语音模块-ESP-SR.md 第 2/3 节）：
  1. 先喊唤醒词「你好小智」→ 输出 wake JSON；
  2. 6 秒内说命令词（25 条之一）→ 输出 cmd JSON（含对应的 MQTT 设备指令）；
  3. 每条命令命中后窗口顺延 6 秒（连续命令模式），静默 6 秒自动回休眠。

用法：
  python voice_debug.py                      # 麦克风 + 唤醒词 + 命令词（默认）
  python voice_debug.py --no-wake            # 跳过唤醒词，直接说命令
  python voice_debug.py --say 打开厨房灯      # 不碰麦克风，直接注入一条命令（模拟固件 say）
  python voice_debug.py --list               # 打印 25 条命令词表
  python voice_debug.py --list-devices       # 列出音频设备（--device 选编号）
  python voice_debug.py --wake 小智小智       # 自定义唤醒词
  python voice_debug.py --device 1           # 选麦克风设备编号
  python voice_debug.py --model D:\\vosk-cn   # 指定模型目录（缺省自动下载到
                                             # %LOCALAPPDATA%\\vosk-models，必须不含中文）

输出 JSON（每行一条，可直接喂给别的程序）：
  {"ts": 1717200000, "event": "wake", "word": "你好小智"}
  {"ts": 1717200000, "event": "cmd", "voice_cmd": "led_kitchen_on",
   "text": "打开厨房灯", "raw": "打开厨房灯",
   "mqtt": {"dev": "led_kitchen", "action": "on"}}
  {"ts": 1717200000, "event": "timeout"}
  {"ts": 1717200000, "event": "unknown", "text": "……"}

依赖安装：
  pip install vosk sounddevice
  中文模型（约 42MB）首次运行自动下载；手动下载：
  https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip
  解压后把 vosk-model-small-cn 目录放到本脚本旁边，或用 --model 指定。

注意：
  · vosk 是通用识别（非命令词专用），唤醒词/命令词按【文本包含】匹配，
    识别率不如固件的 MultiNet 精确模型，本地调试够用；
  · 若 Windows 控制台中文乱码：先运行 `chcp 65001`，或使用 Windows Terminal。
"""

import argparse
import json
import os
import sys
import time
import zipfile
from pathlib import Path
from urllib.request import urlretrieve

WAKE_DEFAULT = "你好小智"
WINDOW_SECONDS = 6.0            # 与固件 VOICE_SR_MN_DURATION_MS 一致
MODEL_URL = "https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip"
MODEL_DIR_NAME = "vosk-model-small-cn"

# ---------------------------------------------------------------------------
# 命令词表 —— 与 components/BSP/VOICE/voice_esp_sr.c 的 s_cmds[] 一一对应
# 三元组：(中文命令词, voice_cmd 名, 对应的 MQTT 指令 JSON)
# mqtt 字段的格式照抄 docs/05-MQTT协议.md 第 3 节（auto 不需要 dev）。
# ---------------------------------------------------------------------------
CMDS = [
    ("打开客厅灯", "led_living_on",  {"dev": "led_living", "action": "on"}),
    ("关闭客厅灯", "led_living_off", {"dev": "led_living", "action": "off"}),
    ("打开厨房灯", "led_kitchen_on",  {"dev": "led_kitchen", "action": "on"}),
    ("关闭厨房灯", "led_kitchen_off", {"dev": "led_kitchen", "action": "off"}),
    ("打开卧室灯", "led_bedroom_on",  {"dev": "led_bedroom", "action": "on"}),
    ("关闭卧室灯", "led_bedroom_off", {"dev": "led_bedroom", "action": "off"}),
    ("打开浴室灯", "led_bath_on",     {"dev": "led_bath", "action": "on"}),
    ("关闭浴室灯", "led_bath_off",    {"dev": "led_bath", "action": "off"}),
    ("打开全部灯", "led_all_on",      {"dev": "all", "action": "on"}),
    ("关闭全部灯", "led_all_off",     {"dev": "all", "action": "off"}),
    ("打开风扇",   "fan_on",           {"dev": "fan", "action": "on"}),
    ("关闭风扇",   "fan_off",          {"dev": "fan", "action": "off"}),
    ("打开窗户",   "window_open",      {"dev": "window", "action": "open"}),
    ("关闭窗户",   "window_close",     {"dev": "window", "action": "close"}),
    ("打开门",     "door_open",        {"dev": "door", "action": "open"}),
    ("关上门",     "door_close",       {"dev": "door", "action": "close"}),
    ("拉开窗帘",   "curtain_open",     {"dev": "curtain", "action": "open"}),
    ("拉上窗帘",   "curtain_close",    {"dev": "curtain", "action": "close"}),
    ("温度多少",   "query_temp",       {"action": "query", "what": "temperature"}),
    ("湿度多少",   "query_humi",       {"action": "query", "what": "humidity"}),
    ("光照多少",   "query_light",      {"action": "query", "what": "light"}),
    ("播报全部",   "query_all",        {"action": "query", "what": "all"}),
    ("状态如何",   "query_status",     {"action": "query", "what": "status"}),
    ("打开自动",   "auto_on",          {"action": "auto", "value": True}),
    ("关闭自动",   "auto_off",         {"action": "auto", "value": False}),
]


def norm(text: str) -> str:
    """归一化：去掉空白和常见标点，用于匹配（vosk 输出可能带标点）。"""
    for ch in "，。！？、,.;;:：\"' \t":
        text = text.replace(ch, "")
    return text.strip()


def match_command(text: str):
    """
    把识别文本匹配到命令词表。
    返回 (voice_cmd, 中文命令词, mqtt_json) 或 None。
    策略：先精确匹配；再双向包含（识别文本包含命令词 或 命令词包含识别文本），
    多条候选时取【命令词最长】的（"打开客厅灯" 比 "打开门" 更具体）。
    """
    t = norm(text)
    if not t:
        return None
    hits = []
    for cn, vc, mqtt in CMDS:
        c = norm(cn)
        if t == c:
            hits.append((vc, cn, mqtt, 100))
        elif c in t or t in c:
            hits.append((vc, cn, mqtt, len(c)))
    if not hits:
        return None
    hits.sort(key=lambda h: -h[3])
    vc, cn, mqtt, _ = hits[0]
    return vc, cn, mqtt


def emit(event: str, **fields):
    """输出一行 JSON（UTF-8，字段顺序固定方便阅读）。"""
    obj = {"ts": int(time.time()), "event": event}
    obj.update(fields)
    print(json.dumps(obj, ensure_ascii=False), flush=True)


# ---------------------------------------------------------------------------
# 模型定位与下载
# ---------------------------------------------------------------------------
def _is_model_dir(p: Path) -> bool:
    """vosk 模型目录的判据：存在 am 子目录。"""
    return p.is_dir() and (p / "am").is_dir()


def _default_model_home() -> Path:
    """模型默认存放位置。
    ★ 必须是纯 ASCII 路径：vosk 的 C++ 加载器在 Windows 上打不开含中文的
    路径（实测本项目路径含"智能家居"时 Model() 报 Failed to create a model）。
    %LOCALAPPDATA% = C:\\Users\\<名>\\AppData\\Local，天然纯 ASCII。"""
    return Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "vosk-models"


def find_model(args) -> str:
    """按优先级找模型：--model > LOCALAPPDATA\\vosk-models > 脚本旁/项目根。
    兼容手动下载后的目录名（vosk-model-small-cn 或 vosk-model-small-cn-0.22）。
    ⚠ 若项目路径含中文，脚本旁/项目根这两个候选 vosk 会打不开，优先用前两个。"""
    names = [MODEL_DIR_NAME, MODEL_DIR_NAME + "-0.22"]
    candidates = []
    if args.model:
        candidates.append(Path(args.model))
    home = _default_model_home()
    for n in names:
        candidates.append(home / n)
    script_dir = Path(__file__).parent
    for n in names:
        candidates.append(script_dir / n)
        candidates.append(script_dir.parent / n)
    for p in candidates:
        if _is_model_dir(p):
            return str(p)
    return ""


def download_model(target_dir: Path) -> bool:
    """下载并解压 vosk 中文小模型到 target_dir 下；返回是否成功。"""
    zip_path = target_dir / (MODEL_DIR_NAME + ".zip")
    print(f"首次运行需要中文识别模型（约 42MB），正在下载到 {zip_path} ...")
    try:
        def hook(blocks, block_size, total):
            if total > 0:
                done = min(blocks * block_size, total)
                print(f"\r  下载中 {done / 1048576:.1f}/{total / 1048576:.1f} MB "
                      f"({done * 100 // total}%)", end="", flush=True)
        urlretrieve(MODEL_URL, zip_path, reporthook=hook)
        print("\n  解压中 ...")
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(target_dir)
        zip_path.unlink(missing_ok=True)
        # 压缩包内顶层目录名带版本号（vosk-model-small-cn-0.22），统一改回
        # 无版本号的名字，方便 find_model 查找。
        extracted = target_dir / (MODEL_DIR_NAME + "-0.22")
        renamed = target_dir / MODEL_DIR_NAME
        if extracted.is_dir() and not renamed.exists():
            extracted.rename(renamed)
        return True
    except KeyboardInterrupt:
        print("\n  下载被中断")
        return False
    except Exception as e:  # 网络失败等
        print(f"\n  下载失败：{e}")
        return False


# ---------------------------------------------------------------------------
# 麦克风实时识别
# ---------------------------------------------------------------------------
def run_mic(args, model_path: str):
    import sounddevice as sd

    if args.list_devices:
        print(sd.query_devices())
        return

    from vosk import Model, KaldiRecognizer

    model = Model(model_path)
    rec = KaldiRecognizer(model, 16000)

    wake_norm = norm(args.wake)
    need_wake = not args.no_wake
    window_sec = args.window
    awake = not need_wake               # --no-wake 时一直处于"听命令"状态
    # --no-wake 时窗口永不超时（inf）；否则初始为 0，等唤醒词打开窗口
    window_deadline = float("inf") if not need_wake else 0.0
    last_partial = ""

    def on_audio(indata, frames, t_info, status):
        nonlocal awake, window_deadline, last_partial
        if status:
            print(f"  [音频] {status}", file=sys.stderr)
        data = bytes(indata)
        if rec.AcceptWaveform(data):
            text = json.loads(rec.Result()).get("text", "")
            last_partial = ""
            if not text:
                return
            t = norm(text)

            # 1) 唤醒词：休眠状态下识别到唤醒词 → 进窗口
            if not awake and wake_norm and wake_norm in t:
                awake = True
                window_deadline = time.time() + window_sec
                emit("wake", word=args.wake, raw=text)
                return

            # 2) 命令词：唤醒窗口内匹配命令表
            if awake:
                if time.time() > window_deadline:
                    # 窗口已过（超时事件在 run 主循环打），这里只重置状态
                    awake = False
                    return
                hit = match_command(text)
                if hit:
                    vc, cn, mqtt = hit
                    window_deadline = time.time() + window_sec  # 连续命令：顺延
                    emit("cmd", voice_cmd=vc, text=cn, raw=text, mqtt=mqtt)
                else:
                    emit("unknown", text=text)
        else:
            partial = json.loads(rec.PartialResult()).get("partial", "")
            if partial and partial != last_partial:
                last_partial = partial
                print(f"\r  [听] {partial:<40s}", end="", flush=True)

    print(f"设备 {args.device} 开始监听（16000Hz 单声道）")
    if need_wake:
        print(f"请说唤醒词「{args.wake}」，然后 6 秒内说命令词；Ctrl+C 退出")
    else:
        print("已跳过唤醒词，直接说命令词即可；Ctrl+C 退出")

    try:
        with sd.RawInputStream(samplerate=16000, blocksize=8000,
                               device=args.device, dtype="int16",
                               channels=1, callback=on_audio):
            while True:
                time.sleep(0.2)
                # 唤醒窗口超时 → 回休眠（与固件的 ESP_MN_STATE_TIMEOUT 一致）
                if awake and time.time() > window_deadline:
                    awake = False
                    emit("timeout")
    except KeyboardInterrupt:
        print("\n  已退出")
    finally:
        emit("exit")


# ---------------------------------------------------------------------------
# 无麦克风注入模式（模拟固件串口台的 say 命令）
# ---------------------------------------------------------------------------
def run_say(args):
    text = "".join(args.say) if isinstance(args.say, list) else args.say
    print(f"  [模拟语音] {text}")
    if norm(args.wake) and norm(args.wake) in norm(text):
        emit("wake", word=args.wake, raw=text)
    hit = match_command(text)
    if hit:
        vc, cn, mqtt = hit
        emit("cmd", voice_cmd=vc, text=cn, raw=text, mqtt=mqtt)
    else:
        emit("unknown", text=text)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(
        description="ESP32 智能家居语音识别本地调试器（vosk 离线中文）")
    ap.add_argument("--say", nargs="+", help="直接注入一句话（不碰麦克风），如 --say 打开厨房灯")
    ap.add_argument("--no-wake", action="store_true", help="跳过唤醒词，直接说命令词")
    ap.add_argument("--wake", default=WAKE_DEFAULT, help=f"唤醒词（默认「{WAKE_DEFAULT}」）")
    ap.add_argument("--window", type=float, default=WINDOW_SECONDS,
                    help=f"唤醒后命令窗口秒数（默认 {WINDOW_SECONDS}，与固件一致）")
    ap.add_argument("--list", action="store_true", help="打印命令词表后退出")
    ap.add_argument("--list-devices", action="store_true", help="列出音频设备后退出")
    ap.add_argument("--device", type=int, default=None, help="音频输入设备编号")
    ap.add_argument("--model", help="vosk 模型目录（缺省自动查找/下载）")
    ap.add_argument("--no-download", action="store_true", help="禁止自动下载模型")
    args = ap.parse_args()

    # --list：打印命令词表（与固件 voice-test 的格式对应）
    if args.list:
        print("============ 语音命令词表（与固件 s_cmds[] 一致） ============")
        print(f"  唤醒词： 「{args.wake}」")
        print(f"  {'序号':<4} {'中文命令词':<10} {'voice_cmd':<18} MQTT 指令")
        for i, (cn, vc, mqtt) in enumerate(CMDS, 1):
            print(f"  {i:<4} {cn:<10} {vc:<18} {json.dumps(mqtt, ensure_ascii=False)}")
        print("=" * 62)
        return

    # --say：注入模式，不需要模型和麦克风
    if args.say:
        run_say(args)
        return

    # 麦克风模式：定位/下载模型
    try:
        from vosk import Model  # noqa: F401  提前验证依赖
        import sounddevice  # noqa: F401
    except ImportError as e:
        print(f"缺少依赖：{e.name}。请先运行： pip install vosk sounddevice")
        return

    if args.list_devices:
        import sounddevice as sd
        print(sd.query_devices())
        return

    model_path = find_model(args)
    if not model_path:
        if args.no_download:
            print("找不到模型目录，且指定了 --no-download。"
                  "请用 --model 指定，或去掉 --no-download 自动下载。")
            return
        dl_home = _default_model_home()
        dl_home.mkdir(parents=True, exist_ok=True)
        if not download_model(dl_home):
            print(f"\n手动下载：{MODEL_URL}\n"
                  f"解压后把 {MODEL_DIR_NAME} 目录放到 {dl_home} 下（路径必须不含中文）。")
            return
        model_path = find_model(args)
        if not model_path:
            print("模型解压后仍未找到，请检查目录结构。")
            return

    print(f"模型目录：{model_path}")
    run_mic(args, model_path)


if __name__ == "__main__":
    main()
