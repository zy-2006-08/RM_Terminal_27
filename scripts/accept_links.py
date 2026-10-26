#!/usr/bin/env python3
"""链路故障验收：一条一条地断，断言终端的状态灯按时变红、恢复后自己变绿，
而且一条链路断了不拖累其他链路。

    .venv/bin/python scripts/accept_links.py
    RM_TERMINAL_BINARY=build/ubuntu/rm_terminal .venv/bin/python scripts/accept_links.py

脚本自己起 broker、链路开关、模拟服务器、图传、假机械臂和一个离屏终端，跑完全部关掉，
约 2 分钟。判定依据是终端日志里的 link_state 记录（只在状态变化时写），以及模拟的车
回报的机械臂姿态（sim/ArmEcho）。已有模拟器在跑时拒绝启动：两套模拟器挤在同一个
broker 上，断言会被另一套的数据搅乱。
"""

from __future__ import annotations

import json
import os
import platform
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "generated"))

import paho.mqtt.client as mqtt

from sim.constants import (MQTT_PORT, MQTT_PROXY_PORT, SERVER_HOST, T_SIM_ARM_BAD,
                           T_SIM_ARM_ECHO, T_SIM_ARM_JOINTS, T_SIM_ARM_PLUGGED,
                           T_SIM_LINK_CAR, T_SIM_LINK_MQTT, T_SIM_LINK_VIDEO)

LINKS = ("referee", "car", "video", "controller")
NAMES = {"referee": "裁判数据", "car": "车端数据", "video": "图传", "controller": "控制器"}
FAULT_TOPIC = {"mqtt": T_SIM_LINK_MQTT, "car": T_SIM_LINK_CAR, "video": T_SIM_LINK_VIDEO}
LINE = re.compile(r"event=link_state link=(\w+) from=(\w+) to=(\w+) summary=(\S+)")

results: list[tuple[bool, str]] = []


def record(ok: bool, text: str) -> None:
    results.append((ok, text))
    print(f"{'PASS' if ok else 'FAIL'}  {text}", flush=True)


def port_busy(port: int) -> bool:
    with socket.socket() as sock:
        sock.settimeout(0.3)
        return sock.connect_ex((SERVER_HOST, port)) == 0


class TerminalLog:
    def __init__(self, path: Path) -> None:
        self.path = path

    def transitions(self) -> list[tuple[str, str, str]]:
        if not self.path.exists():
            return []
        text = self.path.read_text(encoding="utf-8", errors="replace")
        return [(m.group(1), m.group(3), m.group(4)) for m in LINE.finditer(text)]

    def state(self) -> dict[str, str]:
        latest: dict[str, str] = {}
        for link, level, _ in self.transitions():
            latest[link] = level
        return latest

    def count(self, needle: str) -> int:
        if not self.path.exists():
            return 0
        return self.path.read_text(encoding="utf-8", errors="replace").count(needle)


class Sim:
    def __init__(self) -> None:
        self.echo: dict = {}
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="accept-links")
        self.client.on_message = self._on_message
        self.client.on_connect = lambda c, *_: c.subscribe(T_SIM_ARM_ECHO)

    def _on_message(self, _client, _userdata, message) -> None:
        try:
            self.echo = json.loads(message.payload)
        except ValueError:
            pass

    def connect(self) -> None:
        self.client.connect(SERVER_HOST, MQTT_PORT, keepalive=5)
        self.client.loop_start()

    def send(self, topic: str, payload: bytes, retain: bool = True) -> None:
        self.client.publish(topic, payload, qos=1, retain=retain).wait_for_publish(timeout=3)

    def fault(self, link: str, mode: str) -> None:
        if link == "arm":
            if mode == "bad":
                self.send(T_SIM_ARM_BAD, b"1", retain=False)
            else:
                self.send(T_SIM_ARM_PLUGGED, b"0" if mode == "unplug" else b"1")
        else:
            self.send(FAULT_TOPIC[link], mode.encode())


def wait_until(check, timeout: float) -> float | None:
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        if check():
            return time.monotonic() - start
        time.sleep(0.1)
    return None


def scenario(log: TerminalLog, sim: Sim, title: str, faults: list[tuple[str, str]],
             down: dict[str, float], untouched: tuple[str, ...],
             restores: list[tuple[str, str]], recover_s: float, hold_s: float = 2.0) -> None:
    print(f"\n== {title}", flush=True)
    before = len(log.transitions())
    for link, mode in faults:
        sim.fault(link, mode)
    for link, limit in down.items():
        took = wait_until(lambda link=link: log.state().get(link) == "Down", limit + 2)
        record(took is not None and took <= limit,
               f"{NAMES[link]} 变红：{'没变' if took is None else f'{took:.1f} 秒'}（要求 ≤ {limit:g} 秒）")
    time.sleep(hold_s)
    during = log.transitions()[before:]
    for link in untouched:
        hit = [level for name, level, _ in during if name == link and level == "Down"]
        record(not hit, f"{NAMES[link]} 不受影响（期间{'变红过' if hit else '一直没变红'}）")
    for link, mode in restores:
        sim.fault(link, mode)
    took = wait_until(lambda: all(log.state().get(link) == "Ok" for link in LINKS), recover_s)
    record(took is not None,
           f"恢复后全部变绿：{'没恢复 ' + str(log.state()) if took is None else f'{took:.1f} 秒'}"
           f"（要求 ≤ {recover_s:g} 秒）")


def main() -> int:
    build = "build/macos" if platform.system() == "Darwin" else "build/ubuntu"
    binary = Path(os.environ.get("RM_TERMINAL_BINARY", ROOT / build / "rm_terminal"))
    python = os.environ.get("RM_TERMINAL_PYTHON", str(ROOT / ".venv" / "bin" / "python"))
    if not binary.exists():
        print(f"FAIL 找不到终端 {binary}，先构建", file=sys.stderr)
        return 1
    if port_busy(MQTT_PORT) or port_busy(MQTT_PROXY_PORT):
        print("FAIL 3333/3335 端口已被占用：先关掉正在运行的模拟器（启动.command）", file=sys.stderr)
        return 1

    run_dir = Path(tempfile.mkdtemp(prefix="rm-accept-links."))
    arm_path = run_dir / "arm"
    log = TerminalLog(run_dir / "terminal.log")
    conf = run_dir / "terminal.conf"
    conf.write_text(f"mqtt_host=127.0.0.1\nmqtt_port={MQTT_PROXY_PORT}\n"
                    f"controller_serial={arm_path}\nlog_destination={log.path}\n"
                    "self_robot_id=3\n"
                    f"speech_dir={run_dir / 'no-voice'}\n", encoding="utf-8")
    env = dict(os.environ, RM_SELF_ID="3", QT_QPA_PLATFORM="offscreen")
    procs: list[subprocess.Popen] = []

    def start(name: str, args: list[str]) -> subprocess.Popen:
        out = open(run_dir / f"{name}.log", "w", encoding="utf-8")
        proc = subprocess.Popen(args, cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT)
        procs.append(proc)
        return proc

    try:
        start("broker", ["mosquitto", "-p", str(MQTT_PORT)])
        time.sleep(0.8)
        start("proxy", [python, "-u", "-m", "sim.link_proxy"])
        start("server", [python, "-u", "-m", "sim.match_server"])
        start("video", [python, "-u", "-m", "sim.video_sender"])
        start("arm", [python, "-u", "-m", "sim.fake_arm", "--path", str(arm_path)])
        time.sleep(1.5)
        terminal = start("terminal_stdout", [str(binary), "--config", str(conf), "--no-record"])
        sim = Sim()
        sim.connect()

        took = wait_until(lambda: all(log.state().get(link) == "Ok" for link in LINKS), 20)
        record(took is not None, f"启动后四条链路全部变绿（{log.state()}）")
        if took is None:
            return 1

        sim.send(T_SIM_ARM_JOINTS, json.dumps([10, 20, 30, 40, 50, 60]).encode())
        target = [10, 20, 30, 40, 50, 60]
        took = wait_until(lambda: (sim.echo.get("joints_deg") or [None])[0] is not None and all(
            abs(a - b) < 0.5 for a, b in zip(sim.echo["joints_deg"], target)), 3)
        record(took is not None, f"机械臂姿态经终端转发到车：{sim.echo.get('joints_deg')}")

        scenario(log, sim, "断开裁判数据（网线拔了）", [("mqtt", "cut")], {"referee": 3},
                 ("video",), [("mqtt", "ok")], 5)
        scenario(log, sim, "裁判数据卡住（连接没断但没数据）", [("mqtt", "freeze")],
                 {"referee": 3}, ("video",), [("mqtt", "ok")], 5)
        scenario(log, sim, "停图传", [("video", "off")], {"video": 3},
                 ("referee", "car", "controller"), [("video", "ok")], 10)
        scenario(log, sim, "车端断电", [("car", "off")], {"car": 3},
                 ("referee", "video", "controller"), [("car", "ok")], 5)

        print("\n== 拔机械臂", flush=True)
        sim.fault("arm", "unplug")
        took = wait_until(lambda: sim.echo.get("moving") is False, 2)
        record(took is not None and took <= 1.0,
               f"车上机械臂停住：{'没停' if took is None else f'{took:.1f} 秒'}（要求 ≤ 1 秒，不按最后一帧继续动）")
        before = len(log.transitions())
        took = wait_until(lambda: log.state().get("controller") == "Down", 3)
        record(took is not None and took <= 1.0,
               f"控制器变红：{'没变' if took is None else f'{took:.1f} 秒'}（要求 ≤ 1 秒）")
        time.sleep(2)
        frames = sim.echo.get("frames")
        time.sleep(1)
        record(sim.echo.get("frames") == frames, "拔线期间车上一帧新姿态都没收到")
        hit = [n for n, level, _ in log.transitions()[before:] if n != "controller" and level == "Down"]
        record(not hit, f"其他链路不受影响（{hit or '没有别的变红'}）")
        sim.fault("arm", "plug")
        took = wait_until(lambda: log.state().get("controller") == "Ok", 3)
        record(took is not None, f"插回去自动连上：{'没连上' if took is None else f'{took:.1f} 秒'}")
        took = wait_until(lambda: sim.echo.get("moving") is True, 3)
        record(took is not None, "插回后车上机械臂重新跟随")

        print("\n== 坏帧", flush=True)
        bad_before = log.count("state=bad_frames")
        sim.fault("arm", "bad")
        took = wait_until(lambda: log.count("state=bad_frames") > bad_before, 3)
        record(took is not None, "坏帧被识别并记进日志")
        record(log.state().get("controller") == "Ok", "一帧坏帧不让控制器掉线")

        print("\n== 四条全断", flush=True)
        for link, mode in (("mqtt", "cut"), ("car", "off"), ("video", "off"), ("arm", "unplug")):
            sim.fault(link, mode)
        took = wait_until(lambda: all(log.state().get(link) == "Down" for link in LINKS), 6)
        record(took is not None, f"四条全部变红（{log.state()}）")
        time.sleep(3)
        record(terminal.poll() is None, "四条全断时终端没有崩溃")
        for link, mode in (("mqtt", "ok"), ("car", "ok"), ("video", "ok"), ("arm", "plug")):
            sim.fault(link, mode)
        took = wait_until(lambda: all(log.state().get(link) == "Ok" for link in LINKS), 15)
        record(took is not None,
               f"全部恢复后自己变绿：{'没恢复 ' + str(log.state()) if took is None else f'{took:.1f} 秒'}"
               "（期间状态一直在更新，说明界面线程没卡死）")
        record(terminal.poll() is None, "终端全程在运行")
    finally:
        for proc in reversed(procs):
            proc.terminate()
        for proc in procs:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    failed = [text for ok, text in results if not ok]
    print(f"\nRESULT: {'PASS' if not failed else 'FAIL'}（{len(results) - len(failed)}/{len(results)}）"
          f"  日志: {run_dir}")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
