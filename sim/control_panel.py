#!/usr/bin/env python3
import subprocess
import tkinter as tk
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PY = ROOT / ".venv" / "bin" / "python"
CTRL = ROOT / "sim" / "match_control.py"

ROBOTS = [
    ("红1 重装", 1), ("红2 步兵", 2), ("红3 步兵(自机)", 3), ("红4 空中", 4), ("红5 哨兵", 5),
    ("蓝1 重装", 101), ("蓝2 步兵", 102), ("蓝3 步兵", 103), ("蓝4 空中", 104), ("蓝5 哨兵", 105),
]
ROBOT_IDS = {name: robot_id for name, robot_id in ROBOTS}
MAX_HP = 600

root = tk.Tk()
root.title("RM2027 模拟器控制台")

status = tk.Label(root, text="就绪", anchor="w", width=44)


def send(*args: str) -> None:
    try:
        result = subprocess.run(
            [str(PY), str(CTRL), *args], capture_output=True, text=True, timeout=10
        )
    except subprocess.TimeoutExpired:
        status.config(text="发送超时,检查 Broker 是否在运行")
        return
    status.config(text=(result.stdout or result.stderr).strip())


def button(parent, text, *args):
    return tk.Button(parent, text=text, width=8, command=lambda: send(*args))


schedule = tk.LabelFrame(root, text="赛程")
schedule.pack(fill="x", padx=8, pady=6)

row = tk.Frame(schedule)
row.pack(pady=4)
button(row, "开赛", "start").pack(side="left", padx=2)
button(row, "暂停", "pause").pack(side="left", padx=2)
button(row, "复位", "reset").pack(side="left", padx=2)

seek_row = tk.Frame(schedule)
seek_row.pack(pady=4)
tk.Label(seek_row, text="跳到第").pack(side="left")
seek_entry = tk.Entry(seek_row, width=6)
seek_entry.insert(0, "60")
seek_entry.pack(side="left")
tk.Label(seek_row, text="秒").pack(side="left")
tk.Button(seek_row, text="跳转", command=lambda: send("seek", seek_entry.get())).pack(side="left", padx=4)

speed_row = tk.Frame(schedule)
speed_row.pack(pady=4)
tk.Label(speed_row, text="倍速").pack(side="left")
for value in ("1", "2", "5", "10"):
    tk.Button(speed_row, text=f"{value}x", width=4,
              command=lambda v=value: send("speed", v)).pack(side="left", padx=2)

robot = tk.LabelFrame(root, text="机器人")
robot.pack(fill="x", padx=8, pady=6)

robot_row = tk.Frame(robot)
robot_row.pack(pady=4)
selected = tk.StringVar(value=ROBOTS[0][0])
tk.OptionMenu(robot_row, selected, *[name for name, _ in ROBOTS]).pack(side="left")
hp_entry = tk.Entry(robot_row, width=6)
hp_entry.insert(0, str(MAX_HP))
hp_entry.pack(side="left", padx=4)
tk.Button(robot_row, text="设置",
          command=lambda: send("hp", str(ROBOT_IDS[selected.get()]), hp_entry.get())).pack(side="left", padx=2)
tk.Button(robot_row, text="阵亡",
          command=lambda: send("hp", str(ROBOT_IDS[selected.get()]), "0")).pack(side="left", padx=2)
tk.Button(robot_row, text="复活",
          command=lambda: send("hp", str(ROBOT_IDS[selected.get()]), str(MAX_HP))).pack(side="left", padx=2)

auto_hp = tk.BooleanVar(value=True)
tk.Checkbutton(robot, text="随机掉血(关掉后手动摆的血量不会被覆盖)",
               variable=auto_hp,
               command=lambda: send("auto", "1" if auto_hp.get() else "0")).pack(pady=(0, 4))

status.pack(fill="x", padx=8, pady=(0, 8))
root.mainloop()
