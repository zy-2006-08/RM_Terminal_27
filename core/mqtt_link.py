"""终端侧 MQTT + Protobuf 客户端。

上行订阅服务器/机器人数据，下行发送 CustomControl（-> 机器人）与
CommonCommand（-> 服务器）。所有状态带时间戳，供 UI 做失联判定。
"""

from __future__ import annotations

import sys
import threading
import time
from dataclasses import dataclass, field

import paho.mqtt.client as mqtt

import rm_terminal_pb2 as pb
from core.constants import (
    MQTT_PORT,
    SERVER_HOST,
    SUB_TOPICS,
    T_COMMON_COMMAND,
    T_CUSTOM_CONTROL,
    T_EVENT,
    T_GAME_STATUS,
    T_ROBOT_DYNAMIC,
    T_ROBOT_MODULE,
    T_ROBOT_POSITION,
    T_TELEMETRY,
    TELEMETRY_TIMEOUT_S,
)


@dataclass
class TerminalState:
    connected: bool = False
    game: pb.GameStatus = field(default_factory=pb.GameStatus)
    dynamic: pb.RobotDynamicStatus = field(default_factory=pb.RobotDynamicStatus)
    module: pb.RobotModuleStatus = field(default_factory=pb.RobotModuleStatus)
    position: pb.RobotPosition = field(default_factory=pb.RobotPosition)
    telemetry: pb.RobotTelemetry = field(default_factory=pb.RobotTelemetry)
    events: list[tuple[float, int, str]] = field(default_factory=list)

    telemetry_time: float = 0.0
    telemetry_hz: float = 0.0
    messages_received: int = 0
    commands_sent: int = 0

    @property
    def telemetry_online(self) -> bool:
        return self.telemetry_time > 0 and (time.monotonic() - self.telemetry_time) < TELEMETRY_TIMEOUT_S


class MqttLink:
    def __init__(self, host: str = SERVER_HOST, port: int = MQTT_PORT, client_id: str = "rm-terminal") -> None:
        self._host = host
        self._port = port
        self.state = TelemetryGuard(TerminalState())
        self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self._hz_window: list[float] = []
        self._blocked_attempts = 0

    def start(self) -> None:
        self._client.loop_start()
        threading.Thread(target=self._connect_forever, daemon=True).start()

    def stop(self) -> None:
        self._client.loop_stop()
        try:
            self._client.disconnect()
        except Exception:
            pass

    def _connect_forever(self) -> None:
        while True:
            try:
                self._client.connect(self._host, self._port, keepalive=15)
                return
            except OSError:
                time.sleep(1.0)

    def _on_connect(self, client, userdata, flags, reason_code, properties=None) -> None:
        with self.state as s:
            s.connected = True
        client.subscribe([(t, 0) for t in SUB_TOPICS])

    def _on_disconnect(self, client, userdata, flags, reason_code, properties=None) -> None:
        with self.state as s:
            s.connected = False

    def _on_message(self, client, userdata, message) -> None:
        topic, payload = message.topic, message.payload
        now = time.monotonic()
        with self.state as s:
            s.messages_received += 1
            if topic == T_TELEMETRY:
                s.telemetry.ParseFromString(payload)
                s.telemetry_time = now
                self._hz_window.append(now)
                cutoff = now - 1.0
                while self._hz_window and self._hz_window[0] < cutoff:
                    self._hz_window.pop(0)
                s.telemetry_hz = float(len(self._hz_window))
            elif topic == T_GAME_STATUS:
                s.game.ParseFromString(payload)
            elif topic == T_ROBOT_DYNAMIC:
                s.dynamic.ParseFromString(payload)
            elif topic == T_ROBOT_MODULE:
                s.module.ParseFromString(payload)
            elif topic == T_ROBOT_POSITION:
                s.position.ParseFromString(payload)
            elif topic == T_EVENT:
                ev = pb.Event()
                ev.ParseFromString(payload)
                s.events.append((now, ev.level, ev.text))
                del s.events[:-60]

    def send_custom_control(self, command_id: int, param: int = 0) -> bool:
        """阶段一只读：控制下发已硬阻断，恒返回 False 且不发布任何消息。

        原实现会 publish 到 CustomControl（急停、复位云台、开关自瞄、底盘模式），
        与计划的「不实现任何真实机器人控制」冲突。调用点保留但不再产生任何出站流量。
        """
        return self._blocked("CustomControl", command_id, param)

    def send_common_command(self, command_id: int, param: int = 0) -> bool:
        """阶段一只读：控制下发已硬阻断，恒返回 False 且不发布任何消息。"""
        return self._blocked("CommonCommand", command_id, param)

    def _blocked(self, topic: str, command_id: int, param: int) -> bool:
        self._blocked_attempts += 1
        print(f"[readonly_block] 拒绝控制下发 topic={topic} "
              f"command_id={command_id} param={param} "
              f"attempts={self._blocked_attempts}", file=sys.stderr)
        return False


class TelemetryGuard:
    """把状态对象包成上下文管理器，保证 UI 线程与 MQTT 回调线程互斥访问。"""

    def __init__(self, state: TerminalState) -> None:
        self._state = state
        self._lock = threading.Lock()

    def __enter__(self) -> TerminalState:
        self._lock.acquire()
        return self._state

    def __exit__(self, *exc) -> None:
        self._lock.release()

    def snapshot(self) -> TerminalState:
        with self._lock:
            return self._state
