"""从命令行切换模拟链路故障。控制面板「链路」一栏的按钮做的是同一件事。

    .venv/bin/python -m sim.links mqtt cut|freeze|ok   # 裁判数据：断开 / 卡住 / 恢复
    .venv/bin/python -m sim.links car off|ok           # 车端数据：断电 / 恢复
    .venv/bin/python -m sim.links video off|ok         # 图传：停 / 恢复
    .venv/bin/python -m sim.links arm unplug|plug|bad  # 机械臂：拔线 / 插回 / 发一帧坏帧
    .venv/bin/python -m sim.links arm joints 0 30 -20 0 10 45   # 6 个关节角（度）
    .venv/bin/python -m sim.links reset                # 全部恢复
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import paho.mqtt.client as mqtt

from sim.constants import (MQTT_PORT, SERVER_HOST, T_SIM_ARM_BAD, T_SIM_ARM_JOINTS,
                           T_SIM_ARM_PLUGGED, T_SIM_LINK_CAR, T_SIM_LINK_MQTT,
                           T_SIM_LINK_VIDEO)

_LINKS = {
    "mqtt": (T_SIM_LINK_MQTT, {"cut", "freeze", "ok"}),
    "car": (T_SIM_LINK_CAR, {"off", "ok"}),
    "video": (T_SIM_LINK_VIDEO, {"off", "ok"}),
}


def messages(args: list[str]) -> list[tuple[str, bytes, bool]] | None:
    if args == ["reset"]:
        return [(T_SIM_LINK_MQTT, b"ok", True), (T_SIM_LINK_CAR, b"ok", True),
                (T_SIM_LINK_VIDEO, b"ok", True), (T_SIM_ARM_PLUGGED, b"1", True)]
    if len(args) == 2 and args[0] in _LINKS and args[1] in _LINKS[args[0]][1]:
        return [(_LINKS[args[0]][0], args[1].encode(), True)]
    if len(args) == 2 and args[0] == "arm":
        if args[1] in ("plug", "unplug"):
            return [(T_SIM_ARM_PLUGGED, b"1" if args[1] == "plug" else b"0", True)]
        if args[1] == "bad":
            return [(T_SIM_ARM_BAD, b"1", False)]
    if len(args) == 8 and args[:2] == ["arm", "joints"]:
        try:
            joints = [float(value) for value in args[2:]]
        except ValueError:
            return None
        return [(T_SIM_ARM_JOINTS, json.dumps(joints).encode(), True)]
    return None


def publish(items: list[tuple[str, bytes, bool]]) -> None:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="sim-links-cli")
    client.connect(SERVER_HOST, MQTT_PORT, keepalive=5)
    client.loop_start()
    for topic, payload, retain in items:
        client.publish(topic, payload, qos=1, retain=retain).wait_for_publish(timeout=3)
    client.loop_stop()
    client.disconnect()


def main() -> int:
    items = messages(sys.argv[1:])
    if items is None:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        publish(items)
    except OSError as exc:
        print(f"连不上 Broker {SERVER_HOST}:{MQTT_PORT}：{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
