#!/usr/bin/env python3
"""赛程控制:由操作手决定什么时候开赛。

    .venv/bin/python sim/match_control.py prep    # 跳到 3 分钟准备阶段
    .venv/bin/python sim/match_control.py check   # 跳到 15 秒裁判系统自检
    .venv/bin/python sim/match_control.py countdown  # 跳到 5 秒倒计时
    .venv/bin/python sim/match_control.py start   # 继续赛程(赛外时进入准备)
    .venv/bin/python sim/match_control.py pause   # 暂停,时钟停住
    .venv/bin/python sim/match_control.py reset   # 复位回准备阶段
    .venv/bin/python sim/match_control.py seek 60 # 跳到赛程第 60 秒
    .venv/bin/python sim/match_control.py speed 3 # 3 倍速
    .venv/bin/python sim/match_control.py hp 101 300   # 蓝 1 号重装血量改为 300
    .venv/bin/python sim/match_control.py auto 0       # 关掉随机掉血,便于手动摆状态
    .venv/bin/python sim/match_control.py zone 2 2     # 红基地设为红方完全控制(状态 0-4)
    .venv/bin/python sim/match_control.py hold 1 1 6   # 蓝方前哨区连续激活 6 秒(维持临时控制)
    .venv/bin/python sim/match_control.py structure 0 1234  # 红方基地血量设为 1234
    .venv/bin/python sim/match_control.py fortress 1        # 堡垒设为红方,auto 恢复随机
    .venv/bin/python sim/match_control.py reinforce 1       # 红方消耗 1 次加固机会
    .venv/bin/python sim/match_control.py blind 2 4         # 蓝方中「基地随机移动目标」飞镖
    .venv/bin/python sim/match_control.py blind-clear       # 解除手动致盲
    .venv/bin/python sim/match_control.py shoot 17 10       # 对锁定目标打 10 发 17mm
    .venv/bin/python sim/match_control.py module            # 重装安装 42mm 模块
    .venv/bin/python sim/match_control.py buy 17 100        # 花金币兑换 100 发 17mm
    .venv/bin/python sim/match_control.py pay-revive        # 花金币立即复活自机
    .venv/bin/python sim/match_control.py card              # 模拟自机刷场地交互卡解除虚弱
    .venv/bin/python sim/match_control.py card 101          # 对指定地面机器人刷卡
    .venv/bin/python sim/match_control.py dart-open 1       # 红方在 30s/240s 窗口开闸
    .venv/bin/python sim/match_control.py dart 1 1          # 红方飞镖打蓝方前哨站

配合 RM_HOLD=1 启动 match_server.py:那样服务器起来后停在准备阶段等指令,
不发 start 就一直不开赛。不带 RM_HOLD 则维持既有的启动即开赛行为。
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_ROOT))
sys.path.insert(0, str(_ROOT / "generated"))

import paho.mqtt.client as mqtt

import rm_terminal_pb2 as pb
from sim.constants import MQTT_PORT, SERVER_HOST, T_COMMON_COMMAND
from sim.match_server import (
    CTRL_AUTO_HP, CTRL_BEGIN, CTRL_BLIND, CTRL_BLIND_CLEAR, CTRL_BUY, CTRL_CARD,
    CTRL_CHECK, CTRL_COUNTDOWN, CTRL_DART_OPEN, CTRL_DART_SHOOT, CTRL_HOLD,
    CTRL_MODULE, CTRL_PAY_REVIVE, CTRL_PREP,
    CTRL_RESET, CTRL_RESUME, CTRL_SEEK, CTRL_SET_FORTRESS, CTRL_SET_HP,
    CTRL_SET_STRUCTURE_HP, CTRL_SET_ZONE, CTRL_SHOOT, CTRL_SPEED,
    CTRL_USE_REINFORCE,
)

_ACTIONS = {
    "prep": (CTRL_PREP, "进入准备阶段"),
    "check": (CTRL_CHECK, "进入裁判系统自检"),
    "countdown": (CTRL_COUNTDOWN, "进入五秒倒计时"),
    "start": (CTRL_BEGIN, "继续赛程"),
    "pause": (CTRL_HOLD, "暂停"),
    "resume": (CTRL_RESUME, "继续赛程"),
    "reset": (CTRL_RESET, "复位到赛外"),
    "seek": (CTRL_SEEK, "跳转赛程"),
    "speed": (CTRL_SPEED, "设置倍速"),
    "hp": (CTRL_SET_HP, "设置机器人血量"),
    "auto": (CTRL_AUTO_HP, "随机掉血开关"),
    "zone": (CTRL_SET_ZONE, "设置阵地状态"),
    "hold": (CTRL_SET_ZONE, "持续激活阵地"),
    "structure": (CTRL_SET_STRUCTURE_HP, "设置建筑血量"),
    "fortress": (CTRL_SET_FORTRESS, "设置堡垒占领方"),
    "reinforce": (CTRL_USE_REINFORCE, "消耗加固机会"),
    "blind": (CTRL_BLIND, "手动触发飞镖致盲"),
    "blind-clear": (CTRL_BLIND_CLEAR, "解除手动致盲"),
    "shoot": (CTRL_SHOOT, "对锁定目标射击"),
    "module": (CTRL_MODULE, "安装 42mm 模块"),
    "buy": (CTRL_BUY, "兑换允许发弹量"),
    "pay-revive": (CTRL_PAY_REVIVE, "金币兑换立即复活"),
    "card": (CTRL_CARD, "模拟检测到场地交互卡"),
    "dart-open": (CTRL_DART_OPEN, "开启飞镖闸门"),
    "dart": (CTRL_DART_SHOOT, "发射飞镖"),
}
_NEEDS_VALUE = {"seek": 1.0, "speed": 10.0}


def main() -> int:
    args = sys.argv[1:]
    if not args or args[0] not in _ACTIONS:
        print(f"用法: {Path(sys.argv[0]).name} "
              f"{{{'|'.join(_ACTIONS)}}} [秒|倍速]", file=sys.stderr)
        return 2

    action = args[0]
    command_id, label = _ACTIONS[action]
    param = 0
    hold_seconds = 0.0
    if action in _NEEDS_VALUE:
        try:
            param = int(round(float(args[1]) * _NEEDS_VALUE[action]))
        except (IndexError, ValueError):
            print(f"{action} 需要一个数值参数,例如: {action} 2", file=sys.stderr)
            return 2
    elif action == "hp":
        try:
            robot_id, hp = int(args[1]), int(args[2])
        except (IndexError, ValueError):
            print("hp 需要机器人 ID 和血量,例如: hp 101 300", file=sys.stderr)
            return 2
        param = robot_id * 1000 + hp
    elif action == "auto":
        try:
            param = 1 if args[1] in ("1", "on") else 0
        except IndexError:
            print("auto 需要 0 或 1", file=sys.stderr)
            return 2
    elif action == "zone":
        try:
            zone, state = int(args[1]), int(args[2])
        except (IndexError, ValueError):
            print("zone 需要阵地 0-3 和状态 0-4,例如: zone 2 2", file=sys.stderr)
            return 2
        param = zone * 10 + state
    elif action == "hold":
        try:
            zone, state, seconds = int(args[1]), int(args[2]), float(args[3])
        except (IndexError, ValueError):
            print("hold 需要阵地 0-3、状态 0-4 和秒数,例如: hold 1 1 6", file=sys.stderr)
            return 2
        param = zone * 10 + state
        hold_seconds = max(0.1, seconds)
    elif action == "structure":
        try:
            index, hp = int(args[1]), int(args[2])
        except (IndexError, ValueError):
            print("structure 需要建筑 0-3 和血量,例如: structure 0 1234", file=sys.stderr)
            return 2
        param = index * 10000 + hp
    elif action == "fortress":
        try:
            value = args[1]
        except IndexError:
            print("fortress 需要 0/1/2/auto", file=sys.stderr)
            return 2
        if value == "auto":
            param = 3
        elif value in ("0", "1", "2"):
            param = int(value)
        else:
            print("fortress 需要 0/1/2/auto", file=sys.stderr)
            return 2
    elif action == "reinforce":
        try:
            param = int(args[1])
        except (IndexError, ValueError):
            print("reinforce 需要阵营 1(红) 或 2(蓝)", file=sys.stderr)
            return 2
    elif action == "blind":
        try:
            side, target_type = int(args[1]), int(args[2])
        except (IndexError, ValueError):
            print("blind 需要阵营 1/2 和目标类型 1-5,例如: blind 2 4", file=sys.stderr)
            return 2
        param = side * 10 + target_type
    elif action == "shoot":
        try:
            caliber, count = int(args[1]), int(args[2])
        except (IndexError, ValueError):
            print("shoot 需要口径 17/42 和弹丸数,例如: shoot 17 10", file=sys.stderr)
            return 2
        param = caliber * 100 + count
    elif action == "buy":
        try:
            caliber, rounds = int(args[1]), int(args[2])
        except (IndexError, ValueError):
            print("buy 需要口径 17/42 和发数,例如: buy 17 100", file=sys.stderr)
            return 2
        param = caliber * 10000 + rounds
    elif action == "card":
        if len(args) > 1:
            try:
                param = int(args[1])
            except ValueError:
                print("card 的机器人 ID 必须是整数,例如: card 101", file=sys.stderr)
                return 2
    elif action == "dart-open":
        try:
            param = int(args[1])
        except (IndexError, ValueError):
            print("dart-open 需要阵营 1(红) 或 2(蓝)", file=sys.stderr)
            return 2
    elif action == "dart":
        try:
            side, target_type = int(args[1]), int(args[2])
        except (IndexError, ValueError):
            print("dart 需要阵营 1/2 和目标类型 1-5,例如: dart 1 1", file=sys.stderr)
            return 2
        param = side * 10 + target_type

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="sim-control")
    try:
        client.connect(SERVER_HOST, MQTT_PORT, keepalive=10)
    except OSError as exc:
        print(f"无法连接 Broker {SERVER_HOST}:{MQTT_PORT} -> {exc}", file=sys.stderr)
        return 1

    message = pb.CommonCommand()
    message.command_id = command_id
    message.param = param
    client.loop_start()
    end = time.monotonic() + hold_seconds
    while True:
        info = client.publish(T_COMMON_COMMAND, message.SerializeToString(), qos=0)
        # 必须等 broker 真的收下再退出:发完立刻断开会让消息还在发送队列里就被丢掉。
        info.wait_for_publish(timeout=5)
        if time.monotonic() >= end:
            break
        time.sleep(0.02)
    client.loop_stop()
    client.disconnect()
    print(f"已发送: {label} {' '.join(args[1:])}".strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
