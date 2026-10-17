#!/usr/bin/env python3
"""赛程控制:由操作手决定什么时候开赛。

    .venv/bin/python sim/match_control.py start   # 开赛(从当前冻结点继续)
    .venv/bin/python sim/match_control.py pause   # 暂停,时钟停住
    .venv/bin/python sim/match_control.py reset   # 复位回准备阶段
    .venv/bin/python sim/match_control.py seek 60 # 跳到赛程第 60 秒
    .venv/bin/python sim/match_control.py speed 3 # 3 倍速

配合 RM_HOLD=1 启动 match_server.py:那样服务器起来后停在准备阶段等指令,
不发 start 就一直不开赛。不带 RM_HOLD 则维持既有的启动即开赛行为。
"""

from __future__ import annotations

import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_ROOT))
sys.path.insert(0, str(_ROOT / "generated"))

import paho.mqtt.client as mqtt

import rm_terminal_pb2 as pb
from sim.constants import MQTT_PORT, SERVER_HOST, T_COMMON_COMMAND
from sim.match_server import CTRL_BEGIN, CTRL_HOLD, CTRL_RESET, CTRL_SEEK, CTRL_SPEED

_ACTIONS = {
    "start": (CTRL_BEGIN, "开赛"),
    "pause": (CTRL_HOLD, "暂停"),
    "reset": (CTRL_RESET, "复位到准备阶段"),
    "seek": (CTRL_SEEK, "跳转赛程"),
    "speed": (CTRL_SPEED, "设置倍速"),
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
    if action in _NEEDS_VALUE:
        try:
            param = int(round(float(args[1]) * _NEEDS_VALUE[action]))
        except (IndexError, ValueError):
            print(f"{action} 需要一个数值参数,例如: {action} 2", file=sys.stderr)
            return 2

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
    info = client.publish(T_COMMON_COMMAND, message.SerializeToString(), qos=0)
    # 必须等 broker 真的收下再退出:发完立刻断开会让消息还在发送队列里就被丢掉。
    info.wait_for_publish(timeout=5)
    client.loop_stop()
    client.disconnect()
    print(f"已发送: {label} {args[1]}" if action in _NEEDS_VALUE else f"已发送: {label}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
