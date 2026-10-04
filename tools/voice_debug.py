#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模块：
  语音识别的电脑版调试器。板子上那条语音链是麦克风采音、离线识别唤醒词、
  再认命令词；这里用电脑麦克风和 vosk 离线识别把同样的事做一遍，
  方便改命令词、看 JSON 事件，不用一趟趟烧板子。
  行为跟板子一样：先喊唤醒词，六秒内说命令词，命中就顺延六秒，
  一直没说话就回休眠。命令行能开麦克风、跳过唤醒词、直接注入一句话、
  列命令词表、选音频设备、换模型目录；每条结果打一行 JSON，
  可以直接喂给别的程序。要用 pip install vosk sounddevice。
  命令词表要跟固件一致：一条是中文、代号、对应的设备指令，格式照抄协议文档。
  模型默认放系统目录，而且必须全是英文字母：识别库在 Windows 上打不开带中文的
  路径，放系统目录就天然没问题。
  找模型时挨个地方试着找：先看指定目录，再看系统目录，最后看工程里；
  路径带中文的地方识别库打不开，所以优先前面两个。
  不喊唤醒词的那种一直听着；要喊唤醒词的那种一开始是关着的，等喊了才开。

功能：
  听麦克风认唤醒词
  匹配命令词
  打印 JSON 事件
  能直接注入一句话
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

# 功能：命令词表要跟固件一致
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
    """功能：去掉标点和空格"""
    for ch in "，。！？、,.;;:：\"' \t":
        text = text.replace(ch, "")
    return text.strip()


def match_command(text: str):
    """功能：把听到的话对上命令词"""
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
    """功能：打一行结果出去"""
    obj = {"ts": int(time.time()), "event": event}
    obj.update(fields)
    print(json.dumps(obj, ensure_ascii=False), flush=True)


# 功能：找模型、缺了就下
def _is_model_dir(p: Path) -> bool:
    """功能：有 am 目录才算模型"""
    return p.is_dir() and (p / "am").is_dir()


def _default_model_home() -> Path:
    """功能：模型默认放系统目录"""
    return Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "vosk-models"


def find_model(args) -> str:
    """功能：挨个地方试着找模型"""
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
    """功能：下载并解压中文模型"""
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
        # 功能：目录名去掉版本号
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


# 功能：开麦克风实时听
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
    # 功能：不喊唤醒词就一直听
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

            # 功能：听见唤醒词就开窗口
            if not awake and wake_norm and wake_norm in t:
                awake = True
                window_deadline = time.time() + window_sec
                emit("wake", word=args.wake, raw=text)
                return

            # 功能：窗口里就认命令词
            if awake:
                if time.time() > window_deadline:
                    # 功能：超时了，回休眠
                    awake = False
                    return
                hit = match_command(text)
                if hit:
                    vc, cn, mqtt = hit
                    window_deadline = time.time() + window_sec  # 功能：说话就再等六秒
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
                # 功能：等不到话就回休眠
                if awake and time.time() > window_deadline:
                    awake = False
                    emit("timeout")
    except KeyboardInterrupt:
        print("\n  已退出")
    finally:
        emit("exit")


# 功能：不用麦克风，直接喂一句
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

    # 功能：列命令词表就走人
    if args.list:
        print("============ 语音命令词表（与固件 s_cmds[] 一致） ============")
        print(f"  唤醒词： 「{args.wake}」")
        print(f"  {'序号':<4} {'中文命令词':<10} {'voice_cmd':<18} MQTT 指令")
        for i, (cn, vc, mqtt) in enumerate(CMDS, 1):
            print(f"  {i:<4} {cn:<10} {vc:<18} {json.dumps(mqtt, ensure_ascii=False)}")
        print("=" * 62)
        return

    # 功能：直接注入，不用模型
    if args.say:
        run_say(args)
        return

    # 功能：开设备前先备好模型
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
