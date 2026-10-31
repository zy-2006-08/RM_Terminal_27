#!/usr/bin/env python3
"""RM2027 模拟器控制台。

按钮只负责发命令，界面不会因为命令而变，所以底部回读服务器状态：
不显示真实赛程和血量的话，按完看起来就像没反应。状态读的是模拟器单独发的
sim/State（红蓝绝对视角），不是发给终端的协议消息——协议里的「己方/对方」相对自机。
"""
from __future__ import annotations

import json
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
from sim.constants import (MQTT_PORT, SELF_ROBOT_ID, SERVER_HOST, T_ROBOT_RESPAWN,
                           T_SIM_ARM_BAD, T_SIM_ARM_ECHO, T_SIM_ARM_FEEDBACK,
                           T_SIM_ARM_FOLLOW, T_SIM_ARM_JOINTS, T_SIM_ARM_PLUGGED,
                           T_SIM_AUTO_HP, T_SIM_LINK_CAR, T_SIM_LINK_MQTT,
                           T_SIM_LINK_VIDEO, T_SIM_STATE)

PY = ROOT / ".venv" / "bin" / "python"
CTRL = ROOT / "sim" / "match_control.py"

# 场上有血量的 8 台(2027:1 重装 3/4 步兵 7 哨兵)。指挥手(6)没有血量,坐 6 号时只列出来看。
ROBOTS = [
    ("红1 重装", 1), ("红3 步兵", 3), ("红4 步兵", 4), ("红7 哨兵", 7),
    ("蓝1 重装", 101), ("蓝3 步兵", 103), ("蓝4 步兵", 104), ("蓝7 哨兵", 107),
]
if SELF_ROBOT_ID % 100 == 6:
    ROBOTS.insert(0 if SELF_ROBOT_ID < 100 else 4,
                  ("红6 指挥手" if SELF_ROBOT_ID < 100 else "蓝6 指挥手", SELF_ROBOT_ID))
ROBOTS = [(f"{name}(自机)" if robot_id == SELF_ROBOT_ID else name, robot_id)
          for name, robot_id in ROBOTS]
ROBOT_IDS = {name: robot_id for name, robot_id in ROBOTS}
SELF_NAME = next(name for name, robot_id in ROBOTS if robot_id == SELF_ROBOT_ID)
STAGE_NAMES = {0: "赛外", 1: "准备", 2: "裁判系统自检", 3: "倒计时",
               4: "比赛中", 5: "结算"}
ZONE_STATES = ["未控", "红方临时", "红方完全", "蓝方临时", "蓝方完全"]
ZONE_ALLOWED = [ZONE_STATES[1:], ZONE_STATES[1:],
                ["红方完全", "蓝方临时"], ["蓝方完全", "红方临时"]]
ZONES = ["红方前哨区", "蓝方前哨区", "红方基地区", "蓝方基地区"]
ZONE_SHORT = ["前哨红", "前哨蓝", "基地红", "基地蓝"]
STRUCTURES = ["红方基地", "蓝方基地", "红方前哨站", "蓝方前哨站"]
FORTRESS_STATES = ["自动", "停止激活", "蓝方激活红方堡垒", "红方激活蓝方堡垒"]
FORTRESS_SHORT = ["无", "红方堡垒被激活", "蓝方堡垒被激活"]
BLIND_SIDES = ["红方", "蓝方"]
DART_TARGETS = ["前哨站", "基地固定目标", "基地随机固定目标",
                "基地随机移动目标", "基地末端移动目标"]

state = {"online": False, "stage": None, "remain": 0, "elapsed": 0,
         "changed_at": 0.0, "action": "就绪", "hp": {},
         "zones": [0, 0, 0, 0], "control": (0, 0), "reinforce": (0, 0),
         "base": (0, 10000, 0, 10000), "fortress": (0, False, 0.0), "blind": (0, 0),
         "armor": (False, False), "economy": (0, 0),
         "respawn": (False, 0, 0, False, 0, False), "self": None}


def mmss(seconds: float) -> str:
    whole = max(0, int(seconds))
    return f"{whole // 60}:{whole % 60:02d}"


def parse_time(text: str) -> int:
    text = text.strip()
    if ":" in text:
        minutes, _, rest = text.partition(":")
        return int(minutes) * 60 + int(float(rest))
    return int(float(text))


def logistics(hp: int, max_hp: int) -> str:
    """规则 5.6.1：λ = 1 - 基地已损失血量/20000，限制在 [0.5, 1.0]。"""
    if not max_hp:
        return "--"
    value = 1.0 - (max_hp - hp) / 20000.0
    return f"{max(0.5, min(1.0, value)) * 100:.0f}%"


root = tk.Tk()
root.title("RM2027 模拟器控制台")

status = tk.Label(root, text="", anchor="w", justify="left", relief="groove",
                  bg="#1e1e1e", fg="#e6e6e6")
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


schedule = tk.LabelFrame(root, text="裁判赛程")
schedule.pack(fill="x", padx=8, pady=6)

row = tk.Frame(schedule)
row.pack(pady=4)
button(row, "准备", lambda: act("进入准备阶段", "prep")).pack(side="left", padx=2)
button(row, "自检", lambda: act("进入裁判系统自检", "check")).pack(side="left", padx=2)
button(row, "倒计时", lambda: act("进入倒计时", "countdown")).pack(side="left", padx=2)
button(row, "继续", lambda: act("继续赛程", "resume")).pack(side="left", padx=2)
button(row, "暂停", lambda: act("暂停", "pause")).pack(side="left", padx=2)
button(row, "复位", lambda: act("复位到赛外", "reset")).pack(side="left", padx=2)


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
    target = max(0, target)
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

robot = tk.LabelFrame(root, text="自机射击与机器人")
robot.pack(fill="x", padx=8, pady=6)

robot_row = tk.Frame(robot)
robot_row.pack(pady=4)
selected = tk.StringVar(value=SELF_NAME)
tk.OptionMenu(robot_row, selected, *[name for name, _ in ROBOTS]).pack(side="left")
hp_entry = tk.Entry(robot_row, width=6)
hp_entry.insert(0, "400")
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
button(robot_row, "复活", lambda: set_hp(robot_max_hp(), "设为")).pack(side="left", padx=2)


def robot_max_hp() -> int:
    return state["hp"].get(ROBOT_IDS[selected.get()], (0, 600))[1]


shoot_row = tk.Frame(robot)
shoot_row.pack(pady=(0, 4))
tk.Label(shoot_row, text="射击口径").pack(side="left")
caliber = tk.StringVar(value="17")
tk.OptionMenu(shoot_row, caliber, "17", "42").pack(side="left")
count_entry = tk.Entry(shoot_row, width=4)
count_entry.insert(0, "1")
count_entry.pack(side="left", padx=4)
button(shoot_row, "射击",
       lambda: act("射击锁定目标", "shoot", caliber.get(),
                   count_entry.get())).pack(side="left", padx=2)

tool_row = tk.Frame(robot)
tool_row.pack(pady=(0, 4))
button(tool_row, "装42mm",
       lambda: act("安装 42mm 模块", "module")).pack(side="left", padx=2)
tk.Label(tool_row, text="兑换发数").pack(side="left", padx=(6, 0))
buy_entry = tk.Entry(tool_row, width=5)
buy_entry.insert(0, "100")
buy_entry.pack(side="left", padx=4)
button(tool_row, "买弹",
       lambda: act(f"兑换 {caliber.get()}mm x{buy_entry.get()}",
                   "buy", caliber.get(), buy_entry.get())).pack(side="left", padx=2)
button(tool_row, "买活",
       lambda: act("金币兑换立即复活", "pay-revive")).pack(side="left", padx=2)
# 读条满后自机要「确认复活」才起来(协议 2.2.25)。比赛时由选手端或终端点,模拟器里没有
# 选手端,这颗按钮替它点。
button(tool_row, "确认复活",
       lambda: act("确认复活", "confirm-revive")).pack(side="left", padx=2)
button(tool_row, "自机刷卡",
       lambda: act("自机刷卡解除虚弱", "card")).pack(side="left", padx=2)

auto_hp = tk.BooleanVar(value=False)


def toggle_auto() -> None:
    on = auto_hp.get()
    act(f"随机掉血 → {'开' if on else '关'}", "auto", "1" if on else "0")


tk.Checkbutton(robot, text="随机掉血(关掉后手动设的血量不再变化)",
               variable=auto_hp, command=toggle_auto).pack(pady=(0, 4))

zone = tk.LabelFrame(root, text="测试注入 · 阵地")
zone.pack(fill="x", padx=8, pady=6)

zone_row = tk.Frame(zone)
zone_row.pack(pady=4)
zone_selected = tk.StringVar(value=ZONES[2])
zone_state = tk.StringVar(value=ZONE_ALLOWED[2][0])
tk.OptionMenu(zone_row, zone_selected, *ZONES).pack(side="left")
zone_state_menu = tk.OptionMenu(zone_row, zone_state, *ZONE_ALLOWED[2])
zone_state_menu.pack(side="left", padx=4)


def refresh_zone_states(*_args) -> None:
    options = ZONE_ALLOWED[ZONES.index(zone_selected.get())]
    menu = zone_state_menu["menu"]
    menu.delete(0, "end")
    for name in options:
        menu.add_command(label=name, command=lambda v=name: zone_state.set(v))
    zone_state.set(options[0])


zone_selected.trace_add("write", refresh_zone_states)


def set_zone() -> None:
    index = ZONES.index(zone_selected.get())
    value = ZONE_STATES.index(zone_state.get())
    act(f"{zone_selected.get()} → {zone_state.get()}", "zone", str(index), str(value))


def hold_zone() -> None:
    index = ZONES.index(zone_selected.get())
    value = ZONE_STATES.index(zone_state.get())
    threading.Thread(
        target=act,
        args=(f"{zone_selected.get()} 持续激活 6 秒", "hold",
              str(index), str(value), "6"),
        daemon=True,
    ).start()


button(zone_row, "设置阵地", set_zone).pack(side="left", padx=2)
tk.Button(zone_row, text="持续激活 6s", command=hold_zone).pack(side="left", padx=2)

reinforce_row = tk.Frame(zone)
reinforce_row.pack(pady=(0, 4))
tk.Label(reinforce_row, text="加固机会(模拟器内部,2027 协议不发)").pack(side="left")
button(reinforce_row, "红方消耗",
       lambda: act("红方消耗加固机会", "reinforce", "1")).pack(side="left", padx=2)
button(reinforce_row, "蓝方消耗",
       lambda: act("蓝方消耗加固机会", "reinforce", "2")).pack(side="left", padx=2)

structure = tk.LabelFrame(root, text="测试注入 · 建筑与堡垒")
structure.pack(fill="x", padx=8, pady=6)

structure_row = tk.Frame(structure)
structure_row.pack(pady=4)
structure_selected = tk.StringVar(value=STRUCTURES[0])
structure_entry = tk.Entry(structure_row, width=6)
structure_entry.insert(0, "10000")
tk.OptionMenu(structure_row, structure_selected, *STRUCTURES).pack(side="left")
structure_entry.pack(side="left", padx=4)


def set_structure_hp() -> None:
    try:
        value = int(structure_entry.get())
    except ValueError:
        state["action"] = "建筑血量要填数字"
        return
    index = STRUCTURES.index(structure_selected.get())
    act(f"{structure_selected.get()} 血量设为 {value}",
        "structure", str(index), str(value))


button(structure_row, "设定血量", set_structure_hp).pack(side="left", padx=2)

fortress_row = tk.Frame(structure)
fortress_row.pack(pady=(0, 4))
tk.Label(fortress_row, text="堡垒(被激活满 10 秒护甲展开)").pack(side="left")
fortress_selected = tk.StringVar(value=FORTRESS_STATES[1])
tk.OptionMenu(fortress_row, fortress_selected, *FORTRESS_STATES).pack(side="left")


def set_fortress() -> None:
    name = fortress_selected.get()
    value = "auto" if name == "自动" else str(FORTRESS_STATES.index(name) - 1)
    act(f"堡垒 → {name}", "fortress", value)


button(fortress_row, "设置堡垒", set_fortress).pack(side="left", padx=2)

dart = tk.LabelFrame(root, text="飞镖")
dart.pack(fill="x", padx=8, pady=6)
blind_row = tk.Frame(dart)
blind_row.pack(pady=4)
tk.Label(blind_row, text="阵营(中镖/发射)").pack(side="left")
blind_side = tk.StringVar(value=BLIND_SIDES[0])
blind_target = tk.StringVar(value=DART_TARGETS[3])
tk.OptionMenu(blind_row, blind_side, *BLIND_SIDES).pack(side="left")
tk.OptionMenu(blind_row, blind_target, *DART_TARGETS).pack(side="left", padx=4)


def trigger_blind() -> None:
    side = BLIND_SIDES.index(blind_side.get()) + 1
    target = DART_TARGETS.index(blind_target.get()) + 1
    act(f"{blind_side.get()}中镖 {blind_target.get()}",
        "blind", str(side), str(target))


button(blind_row, "中镖", trigger_blind).pack(side="left", padx=2)
button(blind_row, "清账本", lambda: act("清掉模拟器的遮挡账本(终端照常按规则倒计时)",
                                      "blind-clear")).pack(side="left", padx=2)
button(blind_row, "开闸",
       lambda: act("开启飞镖闸门", "dart-open",
                   str(BLIND_SIDES.index(blind_side.get()) + 1))).pack(side="left", padx=2)
button(blind_row, "发射",
       lambda: act("发射飞镖", "dart",
                   str(BLIND_SIDES.index(blind_side.get()) + 1),
                   str(DART_TARGETS.index(blind_target.get()) + 1))).pack(side="left", padx=2)

mark_row = tk.Frame(dart)
mark_row.pack(pady=(0, 4))
tk.Label(mark_row, text="模拟指挥手标记敌方重装").pack(side="left")
for mark_name, mark_kind in (("攻击", "1"), ("防御", "2"), ("警戒", "3")):
    button(mark_row, mark_name,
           lambda k=mark_kind, n=mark_name: act(f"指挥手标记({n})", "mark", k)).pack(side="left", padx=2)


# ---------------- 链路：测终端断线时的反应 ----------------
# 这些按钮不走 match_control.py：故障开关是 retained 消息，由 link_proxy、video_sender、
# fake_arm、match_server 各自订阅，直接用面板自己的 MQTT 连接发出去即可。
LINK_TEXT = {"mqtt": {"ok": "正常", "cut": "断开", "freeze": "卡住"},
             "car": {"ok": "正常", "off": "断电"},
             "video": {"ok": "正常", "off": "停发"}}
LINK_TOPICS = {T_SIM_LINK_MQTT: "mqtt", T_SIM_LINK_CAR: "car", T_SIM_LINK_VIDEO: "video"}
JOINT_NAMES = ["关节0", "关节1", "关节2", "关节3", "夹爪", "臂yaw"]
state.update(links={"mqtt": "ok", "car": "ok", "video": "ok"}, arm_plugged=True,
             arm_echo=None, arm_feedback=None)
panel_client: dict = {"client": None}


def publish(topic: str, payload: bytes, text: str, retain: bool = True) -> None:
    client = panel_client["client"]
    if client is None or not state["online"]:
        state["action"] = "未连接 Broker，链路开关没发出去"
        return
    client.publish(topic, payload, qos=1, retain=retain)
    if text:
        state["action"] = text


links = tk.LabelFrame(root, text="链路（测终端断线时的反应）")
links.pack(fill="x", padx=8, pady=6)
link_row = tk.Frame(links)
link_row.pack(pady=4)
for label, topic, actions in (
        ("裁判数据", T_SIM_LINK_MQTT, (("断开", b"cut"), ("卡住", b"freeze"), ("恢复", b"ok"))),
        ("车端", T_SIM_LINK_CAR, (("断电", b"off"), ("恢复", b"ok"))),
        ("图传", T_SIM_LINK_VIDEO, (("停", b"off"), ("恢复", b"ok")))):
    tk.Label(link_row, text=label).pack(side="left", padx=(8, 0))
    for name, payload in actions:
        button(link_row, name,
               lambda t=topic, p=payload, s=f"{label}：{name}": publish(t, p, s)).pack(
            side="left", padx=2)

arm_row = tk.Frame(links)
arm_row.pack(pady=4)
tk.Label(arm_row, text="假机械臂").pack(side="left")
button(arm_row, "拔线", lambda: publish(T_SIM_ARM_PLUGGED, b"0", "机械臂：拔线")).pack(
    side="left", padx=2)
button(arm_row, "插回", lambda: publish(T_SIM_ARM_PLUGGED, b"1", "机械臂：插回")).pack(
    side="left", padx=2)
button(arm_row, "坏帧", lambda: publish(T_SIM_ARM_BAD, b"1", "机械臂：发一帧坏帧",
                                       retain=False)).pack(side="left", padx=2)
button(arm_row, "车让控制器跟随", lambda: publish(T_SIM_ARM_FOLLOW, b"1", "车的回传标志 2：控制器跟随车")).pack(
    side="left", padx=2)
button(arm_row, "取消跟随", lambda: publish(T_SIM_ARM_FOLLOW, b"0", "车的回传标志 1：正常")).pack(
    side="left", padx=2)


def restore_links() -> None:
    for topic in (T_SIM_LINK_MQTT, T_SIM_LINK_CAR, T_SIM_LINK_VIDEO):
        publish(topic, b"ok", "")
    publish(T_SIM_ARM_FOLLOW, b"0", "")
    publish(T_SIM_ARM_PLUGGED, b"1", "链路全部恢复")


button(arm_row, "全部恢复", restore_links).pack(side="left", padx=(12, 2))

joint_scales: list[tk.Scale] = []


def send_joints(_value: str = "") -> None:
    publish(T_SIM_ARM_JOINTS, json.dumps([scale.get() for scale in joint_scales]).encode(), "")


for start in (0, 3):
    joint_row = tk.Frame(links)
    joint_row.pack(pady=2)
    for name in JOINT_NAMES[start:start + 3]:
        tk.Label(joint_row, text=name, width=5).pack(side="left")
        scale = tk.Scale(joint_row, from_=-180, to=180, orient="horizontal", length=140,
                         command=send_joints)
        scale.pack(side="left", padx=(0, 8))
        joint_scales.append(scale)


def arm_text() -> str:
    plugged = "插着" if state["arm_plugged"] else "拔了"
    echo = state["arm_echo"]
    if not echo or echo.get("joints_deg") is None:
        car = "车上机械臂 还没收到"
    elif echo.get("moving"):
        angles = " ".join(f"{value:.0f}°" for value in echo["joints_deg"])
        car = f"车上机械臂 跟随中 {angles}"
    else:
        age_ms = echo.get("age_ms") or 0
        age = f"{age_ms} ms" if age_ms < 1000 else f"{age_ms // 1000} 秒"
        car = f"车上机械臂 停住（{age}没收到新姿态）"
    feedback = state["arm_feedback"]
    if not feedback or not feedback.get("frames"):
        back = "控制器 还没收到车的回传"
    else:
        flag = "跟随车" if feedback.get("flag") == 2 else "正常"
        back = f"控制器收到车的回传 {feedback['frames']} 帧（{flag}）"
    return f"假机械臂 {plugged} · {car} · {back}"


def on_message(_client, _userdata, message) -> None:
    if message.topic in LINK_TOPICS:
        state["links"][LINK_TOPICS[message.topic]] = message.payload.decode(errors="replace")
        return
    if message.topic == T_SIM_ARM_PLUGGED:
        state["arm_plugged"] = message.payload != b"0"
        return
    if message.topic == T_SIM_ARM_FEEDBACK:
        try:
            state["arm_feedback"] = json.loads(message.payload)
        except ValueError:
            pass
        return
    if message.topic == T_SIM_ARM_ECHO:
        try:
            state["arm_echo"] = json.loads(message.payload)
        except ValueError:
            pass
        return
    if message.topic == T_SIM_STATE:
        try:
            sim = json.loads(message.payload)
        except ValueError:
            return
        if sim["remain"] != state["remain"]:
            state["changed_at"] = time.monotonic()
        state["stage"] = sim["stage"]
        state["remain"] = sim["remain"]
        state["elapsed"] = sim["elapsed"]
        state["zones"] = sim["zones"]
        state["control"] = tuple(sim["control"])
        state["reinforce"] = tuple(sim["reinforce"])
        state["base"] = (sim["base"][0], 10000, sim["base"][1], 10000)
        state["fortress"] = tuple(sim["fortress"])
        state["armor"] = tuple(sim["armor"])
        state["economy"] = tuple(sim["economy"])
        state["blind"] = (sim["blind"][0], sim["blind"][1])
        state["hp"] = {int(rid): tuple(value) for rid, value in sim["hp"].items()}
        state["self"] = {key: sim.get(key) for key in
                         ("performance", "combat", "self_ammo", "rune", "rune_chances", "km")}
        if None in (state["self"]["performance"], state["self"]["combat"]):
            state["self"] = None
        if SELF_ROBOT_ID not in state["hp"]:
            state["hp"][SELF_ROBOT_ID] = tuple(sim["self_hp"])
    elif message.topic == T_ROBOT_RESPAWN:
        resp = pb.RobotRespawnStatus()
        try:
            resp.ParseFromString(message.payload)
        except Exception:
            return
        state["respawn"] = (resp.is_pending_respawn,
                            resp.total_respawn_progress,
                            resp.current_respawn_progress,
                            resp.can_free_respawn,
                            resp.gold_cost_for_respawn,
                            resp.can_pay_for_respawn)
    elif message.topic == T_SIM_AUTO_HP:
        auto_hp.set(message.payload == b"1")


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
        client.subscribe([(T_SIM_STATE, 0), (T_SIM_AUTO_HP, 0),
                          (T_ROBOT_RESPAWN, 0), (T_SIM_LINK_MQTT, 0), (T_SIM_LINK_CAR, 0),
                          (T_SIM_LINK_VIDEO, 0), (T_SIM_ARM_PLUGGED, 0), (T_SIM_ARM_ECHO, 0),
                          (T_SIM_ARM_FEEDBACK, 0)])
        panel_client["client"] = client
        client.loop_forever(retry_first_connection=True)


PERFORMANCE = {1: ("血量优先", "冷却优先"), 2: ("功率优先", "爆发优先")}
RUNE_STATES = {1: "未激活", 2: "激活中", 3: "已激活"}


def self_text() -> str:
    """自机:允许发弹量、脱战、性能体系、能量机关。终端「本车指令」面板发的指令改的就是这些。"""
    extra = state["self"]
    if not extra:
        return "自机 等待服务器状态"
    chassis, shooter = extra["performance"]
    out, countdown = extra["combat"]
    return (f"自机 弹 {extra['self_ammo']} · {'脱战' if out else f'交战中 {countdown}s'}"
            f" · 底盘 {PERFORMANCE.get(chassis, ('?',))[0]} · 发射 {PERFORMANCE.get(shooter, ('?', '?'))[1]}"
            f" · 能量机关 {RUNE_STATES.get(extra['rune'], extra['rune'])} 机会 {extra['rune_chances']}")


def km_text() -> str:
    """终端发来的键鼠控制(2.2.1):最近一秒的帧率、按着的键、鼠标位移。"""
    km = state["self"].get("km") if state["self"] else None
    if not km:
        return "键鼠 终端没在发"
    held = " ".join(part for part in (km["keys"], km["buttons"] and f"{km['buttons']}键") if part) or "无按键"
    x, y, z = km["mouse"]
    return f"键鼠 {km['hz']} 帧/秒 · {held} · 鼠标 x{x:+d} y{y:+d} 滚轮{z:+d}"


def tick() -> None:
    if not state["online"]:
        head = "未连接 Broker(先双击 启动.command)"
    elif state["stage"] is None:
        head = "已连接,等待服务器状态"
    else:
        stage = STAGE_NAMES.get(state["stage"], f"阶段 {state['stage']}")
        head = f"{stage} · 剩余 {mmss(state['remain'])}"
        head += f" · 已过 {mmss(state['elapsed'])}"
        if time.monotonic() - state["changed_at"] > 2.0:
            head += " · 停表"
    robot_id = ROBOT_IDS[selected.get()]
    hp = state["hp"].get(robot_id)
    hp_text = f"{selected.get()} 血量 {hp[0]} / {hp[1]}" if hp else f"{selected.get()} 血量未知"
    hp_text += f" · 随机掉血 {'开' if auto_hp.get() else '关'}"
    red_base, red_max, blue_base, blue_max = state["base"]
    zone_text = "阵地 " + " ".join(
        f"{short}:{ZONE_STATES[value]}"
        for short, value in zip(ZONE_SHORT, state["zones"]))
    fortress_side, fortress_auto, fortress_sec = state["fortress"]
    fortress_text = FORTRESS_SHORT[fortress_side] if 0 <= fortress_side < 3 else "?"
    if fortress_side:
        fortress_text += f" {fortress_sec:.0f}s/10s"
    if fortress_auto:
        fortress_text += "(随机)"
    armor = "/".join(name for name, opened in zip(("红方", "蓝方"), state["armor"]) if opened)
    global_text = (f"控制 红{state['control'][0]} 蓝{state['control'][1]}"
                   f" · 经济 红{state['economy'][0]} 蓝{state['economy'][1]}"
                   f" · 后勤 红{logistics(red_base, red_max)}"
                   f" 蓝{logistics(blue_base, blue_max)}"
                   f" · 堡垒 {fortress_text}"
                   f" · 护甲展开 {armor or '无'}")
    blind_side_value, blind_type = state["blind"]
    if blind_side_value and 1 <= blind_type <= len(DART_TARGETS):
        blind_text = (f"{BLIND_SIDES[blind_side_value - 1]}/"
                      f"{DART_TARGETS[blind_type - 1]}")
    else:
        blind_text = "无"
    pending, total, current, free, cost, can_pay = state["respawn"]
    if pending:
        respawn_text = f"{mmss(current)}/{mmss(total)}"
        respawn_text += (" · 读条满，等确认复活" if free
                         else f" · 买活 {cost} 金币{'够' if can_pay else '不足'}")
    else:
        respawn_text = "无"
    link_text = "链路 " + " · ".join(
        f"{label} {LINK_TEXT[key].get(state['links'][key], state['links'][key])}"
        for label, key in (("裁判数据", "mqtt"), ("车端", "car"), ("图传", "video")))
    status.config(text=f"{head}\n{hp_text}\n{zone_text}\n{global_text}"
                       f" · 致盲 {blind_text} · 复活 {respawn_text}\n"
                       f"{self_text()}\n{km_text()}\n"
                       f"{link_text}\n{arm_text()}\n{state['action']}")
    root.after(250, tick)


threading.Thread(target=subscribe, daemon=True).start()
tick()
root.mainloop()
