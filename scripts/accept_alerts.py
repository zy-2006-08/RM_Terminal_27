#!/usr/bin/env python3
"""事件提醒验收：让模拟器把每种事件演一遍，断言终端的提醒该响的响、只响该响的次数。

    .venv/bin/python scripts/accept_alerts.py

自己起 broker、模拟服务器和一个离屏终端（准备阶段缩短到几秒），跑完全部关掉，约 1 分钟。
判定依据是终端日志里的 `event=alert kind=…` 记录。已有模拟器在跑时拒绝启动。
"""

from __future__ import annotations

import os
import platform
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sim.constants import MQTT_PORT, SERVER_HOST

ALERT = re.compile(r"event=alert kind=(\w+) level=(\w+) text=(.*)$", re.M)
results: list[tuple[bool, str]] = []


def record(ok: bool, text: str) -> None:
    results.append((ok, text))
    print(f"{'PASS' if ok else 'FAIL'}  {text}", flush=True)


def wait_until(check, timeout: float) -> bool:
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        if check():
            return True
        time.sleep(0.1)
    return check()


def main() -> int:
    build = "build/macos" if platform.system() == "Darwin" else "build/ubuntu"
    binary = Path(os.environ.get("RM_TERMINAL_BINARY", ROOT / build / "rm_terminal"))
    python = os.environ.get("RM_TERMINAL_PYTHON", str(ROOT / ".venv" / "bin" / "python"))
    with socket.socket() as sock:
        sock.settimeout(0.3)
        if sock.connect_ex((SERVER_HOST, MQTT_PORT)) == 0:
            print("FAIL 3333 端口已被占用：先关掉正在运行的模拟器（启动.command）", file=sys.stderr)
            return 1

    run_dir = Path(tempfile.mkdtemp(prefix="rm-accept-alerts."))
    log_path = run_dir / "terminal.log"
    conf = run_dir / "terminal.conf"
    conf.write_text(f"mqtt_port={MQTT_PORT}\nlog_destination={log_path}\nself_robot_id=3\n"
                    f"speech_dir={run_dir / 'no-voice'}\n",
                    encoding="utf-8")
    env = dict(os.environ, RM_SELF_ID="3", QT_QPA_PLATFORM="offscreen",
               RM_PREP_SEC="3", RM_CHECK_SEC="1", RM_COUNTDOWN_SEC="1")
    procs: list[subprocess.Popen] = []

    def start(name: str, args: list[str]) -> subprocess.Popen:
        out = open(run_dir / f"{name}.log", "w", encoding="utf-8")
        proc = subprocess.Popen(args, cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT)
        procs.append(proc)
        return proc

    def control(*args: str) -> None:
        subprocess.run([python, "sim/match_control.py", *args], cwd=ROOT, env=env,
                       capture_output=True, timeout=10, check=False)

    def alerts() -> list[tuple[str, str]]:
        if not log_path.exists():
            return []
        text = log_path.read_text(encoding="utf-8", errors="replace")
        return [(m.group(1), m.group(3)) for m in ALERT.finditer(text)]

    def count(kind: str, contains: str = "") -> int:
        return sum(1 for k, text in alerts() if k == kind and contains in text)

    try:
        start("broker", ["mosquitto", "-p", str(MQTT_PORT)])
        time.sleep(0.8)
        start("server", [python, "-u", "-m", "sim.match_server"])
        time.sleep(1)
        terminal = start("terminal_stdout", [str(binary), "--config", str(conf), "--no-record"])
        control("auto", "0")

        print("\n== 开赛", flush=True)
        record(wait_until(lambda: count("energy", "小能量") >= 1, 20),
               "开赛时提醒小能量机关可以激活")

        print("\n== 飞镖闸门（跳到 0:18，3 倍速）", flush=True)
        control("seek", "18")
        control("speed", "3")
        record(wait_until(lambda: count("dart", "10 秒后") == 1 and count("dart", "可以打开了") == 1, 12),
               f"0:20 预告、0:30 开闸各一次（实际 {count('dart')} 次）")
        control("speed", "1")

        print("\n== 能量机关（跳到 1:27 和 2:57）", flush=True)
        control("seek", "87")
        record(wait_until(lambda: count("energy", "小能量") == 2, 8), "1:30 小能量机关第二次机会")
        control("seek", "177")
        record(wait_until(lambda: count("energy", "大能量") == 1, 8), "3:00 大能量机关")

        print("\n== 前哨阵地（敌方连续激活 6 秒，再击毁前哨站）", flush=True)
        # 阵地 0 = 红方前哨区，状态 3 = 蓝方临时控制。
        control("hold", "0", "3", "6")
        record(wait_until(lambda: count("outpost", "临时控制") >= 1, 10), "我方前哨被敌方临时控制时提醒")
        time.sleep(4)
        record(count("outpost", "临时控制") == 1, f"同一次临时控制只提醒一次（实际 {count('outpost', '临时控制')} 次）")
        control("structure", "2", "0")
        ok = wait_until(lambda: count("outpost", "完全控制") == 1, 6)
        record(ok, f"转成完全控制再提醒一次（实际 {count('outpost', '完全控制')} 次）")

        print("\n== 堡垒", flush=True)
        control("fortress", "2")
        record(wait_until(lambda: count("fortress", "敌方占领") == 1, 5), "堡垒被敌方占领")
        control("fortress", "0")
        record(wait_until(lambda: count("fortress", "空出来") == 1, 5), "堡垒空出来了")

        print("\n== 敌方残血", flush=True)
        control("hp", "103", "40")
        record(wait_until(lambda: count("low_hp", "3 号") == 1, 5), "敌方 3 号残血提醒")
        control("hp", "103", "30")
        time.sleep(1.5)
        record(count("low_hp", "3 号") == 1, "继续掉血不重复提醒")
        control("hp", "103", "200")
        time.sleep(1)
        control("hp", "103", "45")
        record(wait_until(lambda: count("low_hp", "3 号") == 2, 5), "回满血后再残血会再提醒")

        print("\n== 我方基地被攻击", flush=True)
        control("structure", "0", "9000")
        record(wait_until(lambda: count("base") == 1, 5), "基地掉血提醒")
        control("structure", "0", "8000")
        time.sleep(2)
        record(count("base") == 1, "10 秒内再掉血不重复提醒")

        print("\n== 控制度落后（敌方占两个前哨阵地，10 倍速）", flush=True)
        control("speed", "10")
        record(wait_until(lambda: count("control") == 1, 20), "控制度落后 30 分提醒")
        time.sleep(3)
        record(count("control") == 1, f"继续落后不重复提醒（实际 {count('control')} 次）")
        control("speed", "1")

        # 模拟器按真实流逝的比赛时间每分钟发一次加固机会，跳转不算，所以放到最后检查。
        print("\n== 加固机会", flush=True)
        wait_until(lambda: count("reinforce") >= 1, 40)
        record(count("reinforce") == 1, f"加固机会从无到有只提醒一次（实际 {count('reinforce')} 次）")
        record(terminal.poll() is None, "终端全程在运行")
    finally:
        for proc in reversed(procs):
            proc.terminate()
        for proc in procs:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    print("\n收到的全部提醒：")
    for kind, text in alerts():
        print(f"  {kind:10s} {text}")
    failed = [text for ok, text in results if not ok]
    print(f"\nRESULT: {'PASS' if not failed else 'FAIL'}（{len(results) - len(failed)}/{len(results)}）"
          f"  日志: {run_dir}")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
