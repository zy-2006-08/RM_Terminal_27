#!/usr/bin/env python3
"""RM2027 模拟器控制台。

按钮只负责发命令，界面不会因为命令而变，所以底部回读服务器状态：
不显示真实赛程和血量的话，按完看起来就像没反应。
"""
from __future__ import annotations

import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "generated"))

import paho.mqtt.client as mqtt

import rm_terminal_pb2 as pb
from sim.constants import (MQTT_PORT, SERVER_HOST, T_GAME_STATUS, T_ROBOT_HEALTH_SET,
                           T_SIM_AUTO_HP)

PY = ROOT / ".venv" / "bin" / "python"
CTRL = ROOT / "sim" / "match_control.py"

ROBOTS = [
    ("红1 重装", 1), ("红2 步兵", 2), ("红3 步兵(自机)", 3), ("红4 空中", 4), ("红5 哨兵", 5),
    ("蓝1 重装", 101), ("蓝2 步兵", 102), ("蓝3 步兵", 103), ("蓝4 空中", 104), ("蓝5 哨兵", 105),
]
ROBOT_IDS = {name: robot_id for name, robot_id in ROBOTS}
MAX_HP = 600
STAGE_NAMES = {3: "准备倒计时", 4: "比赛中", 5: "结算"}
COUNTDOWN_SEC = 4

state = {"online": False, "stage": None, "remain": 0, "elapsed": 0,
         "changed_at": 0.0, "action": "就绪", "hp": {}}


def mmss(seconds: float) -> str:
    whole = max(0, int(seconds))
    return f"{whole // 60}:{whole % 60:02d}"


def parse_time(text: str) -> int:
    text = text.strip()
    if ":" in text:
        minutes, _, rest = text.partition(":")
        return int(minutes) * 60 + int(float(rest))
    return int(float(text))


root = tk.Tk()
root.title("RM2027 模拟器控制台")

status = tk.Label(root, text="", anchor="w", justify="left", relief="groove",
                  bg="white", fg="black")
status.pack(side="bottom", fill="x", padx=8, pady=(2, 6))


def act(text: str, *args: str) -> None:
    try:
        result = subprocess.run(
            [str(PY), str(CTRL), *args], capture_output=True, text=True, timeout=10
        )
    except subprocess.TimeoutExpired:
        state["action"] = "发送超时:检查 Broker 是否在运行"
        return
    state["action"] = text if result.returncode == 0 else (result.stderr or "发送失败").strip()


def button(parent, text, command):
    return tk.Button(parent, text=text, width=8, command=command)


schedule = tk.LabelFrame(root, text="赛程")
schedule.pack(fill="x", padx=8, pady=6)

row = tk.Frame(schedule)
row.pack(pady=4)
button(row, "开赛", lambda: act("开赛", "start")).pack(side="left", padx=2)
button(row, "暂停", lambda: act("暂停", "pause")).pack(side="left", padx=2)
button(row, "复位", lambda: act("复位到准备阶段", "reset")).pack(side="left", padx=2)


def do_seek() -> None:
    """输入的是终端大屏上那个倒计时,不是已过时间。

    服务器只认赛程已过秒数,所以用同一帧的 剩余/已过 换算,不写死 420 秒。
    """
    try:
        remaining = parse_time(seek_entry.get())
    except ValueError:
        state["action"] = "时间填 6:30 或 390"
        return
    if state["stage"] is None:
        state["action"] = "还没收到服务器状态"
        return
    target = state["elapsed"] + (state["remain"] - remaining)
    target = max(COUNTDOWN_SEC, target)
    act(f"比赛时钟设为 {mmss(remaining)}", "seek", str(target))


seek_row = tk.Frame(schedule)
seek_row.pack(pady=4)
tk.Label(seek_row, text="比赛时钟(剩余)").pack(side="left")
seek_entry = tk.Entry(seek_row, width=6)
seek_entry.insert(0, "6:30")
seek_entry.pack(side="left", padx=4)
tk.Label(seek_row, text="分:秒").pack(side="left")
tk.Button(seek_row, text="跳转", command=do_seek).pack(side="left", padx=4)

speed_row = tk.Frame(schedule)
speed_row.pack(pady=4)
tk.Label(speed_row, text="倍速").pack(side="left")
for value in ("1", "2", "5", "10"):
    button(speed_row, f"{value}x",
           lambda v=value: act(f"倍速设为 {v}x", "speed", v)).pack(side="left", padx=2)

robot = tk.LabelFrame(root, text="机器人")
robot.pack(fill="x", padx=8, pady=6)

robot_row = tk.Frame(robot)
robot_row.pack(pady=4)
selected = tk.StringVar(value=ROBOTS[0][0])
tk.OptionMenu(robot_row, selected, *[name for name, _ in ROBOTS]).pack(side="left")
hp_entry = tk.Entry(robot_row, width=6)
hp_entry.insert(0, str(MAX_HP))
hp_entry.pack(side="left", padx=4)


def set_hp(value: int, verb: str) -> None:
    name = selected.get()
    act(f"{name} 血量{verb} {value}", "hp", str(ROBOT_IDS[name]), str(value))


def set_hp_from_entry() -> None:
    try:
        value = int(hp_entry.get())
    except ValueError:
        state["action"] = "血量要填数字"
        return
    set_hp(value, "设为")


button(robot_row, "设定血量", set_hp_from_entry).pack(side="left", padx=2)
button(robot_row, "阵亡", lambda: set_hp(0, "设为")).pack(side="left", padx=2)
button(robot_row, "复活", lambda: set_hp(MAX_HP, "设为")).pack(side="left", padx=2)

auto_hp = tk.BooleanVar(value=True)


def toggle_auto() -> None:
    on = auto_hp.get()
    act(f"随机掉血 → {'开' if on else '关'}", "auto", "1" if on else "0")


tk.Checkbutton(robot, text="随机掉血(关掉后手动设的血量不再变化)",
               variable=auto_hp, command=toggle_auto).pack(pady=(0, 4))


def on_message(_client, _userdata, message) -> None:
    if message.topic == T_GAME_STATUS:
        status_msg = pb.GameStatus()
        try:
            status_msg.ParseFromString(message.payload)
        except Exception:
            return
        if status_msg.stage_countdown_sec != state["remain"]:
            state["changed_at"] = time.monotonic()
        state["stage"] = status_msg.current_stage
        state["remain"] = status_msg.stage_countdown_sec
        state["elapsed"] = status_msg.stage_elapsed_sec
    elif message.topic == T_SIM_AUTO_HP:
        auto_hp.set(message.payload == b"1")
    elif message.topic == T_ROBOT_HEALTH_SET:
        health = pb.RobotHealthSet()
        try:
            health.ParseFromString(message.payload)
        except Exception:
            return
        for entry in health.entries:
            state["hp"][entry.robot_id] = (entry.current_hp, entry.max_hp)


def subscribe() -> None:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="sim-panel")
    client.on_message = on_message
    client.on_connect = lambda *_: state.update(online=True)
    client.on_disconnect = lambda *_: state.update(online=False)
    # 面板比 run.sh 先起来，第一次 connect 必然失败；Broker 被验收脚本关掉后也要能自己回来
    while True:
        try:
            client.connect(SERVER_HOST, MQTT_PORT, keepalive=5)
        except OSError:
            state["online"] = False
            time.sleep(1)
            continue
        client.subscribe([(T_GAME_STATUS, 0), (T_ROBOT_HEALTH_SET, 0), (T_SIM_AUTO_HP, 0)])
        client.loop_forever(retry_first_connection=True)


def tick() -> None:
    if not state["online"]:
        head = "未连接 Broker(先双击 启动.command)"
    elif state["stage"] is None:
        head = "已连接,等待服务器状态"
    else:
        stage = STAGE_NAMES.get(state["stage"], f"阶段 {state['stage']}")
        head = f"{stage} · 剩余 {mmss(state['remain'])}"
        head += f" · 已过 {mmss(state['elapsed'] - COUNTDOWN_SEC)}"
        if time.monotonic() - state["changed_at"] > 2.0:
            head += " · 停表"
    robot_id = ROBOT_IDS[selected.get()]
    hp = state["hp"].get(robot_id)
    hp_text = f"{selected.get()} 血量 {hp[0]} / {hp[1]}" if hp else f"{selected.get()} 血量未知"
    hp_text += f" · 随机掉血 {'开' if auto_hp.get() else '关'}"
    status.config(text=f"{head}\n{hp_text}\n最近操作: {state['action']}")
    root.after(250, tick)


threading.Thread(target=subscribe, daemon=True).start()
tick()
root.mainloop()
