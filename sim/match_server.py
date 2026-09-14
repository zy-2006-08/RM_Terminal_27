"""模拟赛事引擎服务器 + 模拟机器人遥测。

按 2026 协议规定的频率通过 MQTT 发布 Protobuf 消息：
    GameStatus         5Hz
    RobotDynamicStatus 10Hz
    RobotModuleStatus  1Hz
    RobotPosition      1Hz
    Event              触发式
    RobotTelemetry     50Hz（机器人 -> 终端，对应 0x0310）
同时订阅终端下行的 CustomControl / CommonCommand 并打印，验证双向链路。
"""

from __future__ import annotations

import math
import random
import sys
import threading
import time
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_ROOT))
sys.path.insert(0, str(_ROOT / "generated"))

import paho.mqtt.client as mqtt

import rm_terminal_pb2 as pb
from core.constants import (
    MQTT_PORT,
    SELF_ROBOT_ID,
    SERVER_HOST,
    T_COMMON_COMMAND,
    T_CUSTOM_CONTROL,
    T_EVENT,
    T_GAME_STATUS,
    T_ROBOT_DYNAMIC,
    T_ROBOT_MODULE,
    T_ROBOT_POSITION,
    T_TELEMETRY,
)

_CMD_NAMES = {
    0x01: "设置底盘模式",
    0x02: "切换自瞄",
    0x03: "紧急停止",
    0x04: "云台回中",
}


class SimulatedMatch:
    def __init__(self) -> None:
        self.t0 = time.monotonic()
        self.hp = 400
        self.max_hp = 600
        self.heat = 0
        self.ammo = 150
        self.coin = 220
        self.red_score = 0
        self.blue_score = 0
        self.seq = 0
        self.rng = random.Random(2027)
        self.target_locked = False
        self.target_id = 0
        self.vision_online = True
        self.chassis_mode = 2
        self.autoaim = True
        self.events: list[tuple[int, str]] = []

    @property
    def elapsed(self) -> float:
        return time.monotonic() - self.t0

    def stage(self) -> tuple[int, int]:
        """准备 5s -> 倒计时 5s -> 比赛 420s，循环往复便于长时间演示。"""
        e = self.elapsed
        if e < 5:
            return 1, int(5 - e)
        if e < 10:
            return 3, int(10 - e)
        match_elapsed = (e - 10) % 420
        return 4, int(420 - match_elapsed)

    def tick_fast(self) -> None:
        """50Hz：更新云台、自瞄与目标状态。"""
        self.seq += 1
        e = self.elapsed

        if self.rng.random() < 0.004:
            self.vision_online = not self.vision_online
            self.events.append((2 if not self.vision_online else 0,
                                "视觉进程离线" if not self.vision_online else "视觉进程恢复"))

        if self.vision_online and self.autoaim:
            if self.rng.random() < 0.02:
                self.target_locked = not self.target_locked
                if self.target_locked:
                    self.target_id = self.rng.choice([101, 103, 104, 107])
                    self.events.append((1, f"锁定目标 ID {self.target_id}"))
        else:
            self.target_locked = False

        if self.heat > 0:
            self.heat = max(0, self.heat - 2)

    def tick_10hz(self) -> None:
        if self.stage()[0] != 4:
            return
        if self.target_locked and self.ammo > 0 and self.rng.random() < 0.3:
            self.ammo -= 1
            self.heat = min(300, self.heat + 10)
            if self.heat > 240:
                self.events.append((2, "射击热量接近上限"))
        if self.hp == 0:
            if self.rng.random() < 0.04:
                self.hp = self.max_hp
                self.ammo = min(400, self.ammo + 100)
                self.events.append((0, "复活完成，血量与弹量已补充"))
            return

        if self.rng.random() < 0.05:
            dmg = self.rng.randint(5, 40)
            self.hp = max(0, self.hp - dmg)
            if self.hp == 0:
                self.events.append((3, "机器人阵亡，等待复活"))
            else:
                self.events.append((2 if dmg > 25 else 1, f"装甲板受击 -{dmg} HP"))
        if self.rng.random() < 0.03:
            self.hp = min(self.max_hp, self.hp + 60)
            self.coin += 20
        if self.rng.random() < 0.01:
            self.red_score += 1
        if self.rng.random() < 0.008:
            self.blue_score += 1


def _publish(client: mqtt.Client, topic: str, msg) -> None:
    client.publish(topic, msg.SerializeToString(), qos=0)


def _on_connect(client, userdata, flags, reason_code, properties=None):
    print(f"[server] MQTT 已连接 ({reason_code})，订阅终端下行指令")
    client.subscribe([(T_CUSTOM_CONTROL, 0), (T_COMMON_COMMAND, 0)])


def _on_message(client, userdata, message):
    if message.topic == T_CUSTOM_CONTROL:
        m = pb.CustomControl()
        m.ParseFromString(message.payload)
        raw = m.data
        if len(raw) >= 2:
            name = _CMD_NAMES.get(raw[0], f"0x{raw[0]:02X}")
            print(f"[server] 收到 CustomControl <- 终端：{name} 参数={raw[1]} "
                  f"({len(raw)} 字节，上限 30)")
            userdata.apply_command(raw[0], raw[1])
    elif message.topic == T_COMMON_COMMAND:
        m = pb.CommonCommand()
        m.ParseFromString(message.payload)
        print(f"[server] 收到 CommonCommand <- 终端：id={m.command_id} param={m.param}")


def main() -> int:
    match = SimulatedMatch()

    def apply_command(cmd: int, param: int) -> None:
        if cmd == 0x01:
            match.chassis_mode = param
            match.events.append((0, f"底盘模式切换为 {param}"))
        elif cmd == 0x02:
            match.autoaim = bool(param)
            match.events.append((0, f"自瞄{'启用' if param else '关闭'}"))
        elif cmd == 0x03:
            match.chassis_mode = 0
            match.autoaim = False
            match.events.append((3, "终端下发紧急停止"))
        elif cmd == 0x04:
            match.events.append((0, "云台回中"))

    match.apply_command = apply_command

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sim-server-{SELF_ROBOT_ID}",
                         userdata=match)
    client.on_connect = _on_connect
    client.on_message = _on_message

    for attempt in range(20):
        try:
            client.connect(SERVER_HOST, MQTT_PORT, keepalive=30)
            break
        except OSError as exc:
            if attempt == 19:
                print(f"[server] 无法连接 Broker {SERVER_HOST}:{MQTT_PORT} -> {exc}", file=sys.stderr)
                return 1
            time.sleep(0.5)

    client.loop_start()
    print(f"[server] 模拟赛事引擎启动 -> mqtt://{SERVER_HOST}:{MQTT_PORT}，Ctrl-C 退出")

    stop = threading.Event()

    def loop(period: float, fn) -> None:
        due = time.monotonic()
        while not stop.is_set():
            fn()
            due += period
            delay = due - time.monotonic()
            if delay > 0:
                stop.wait(delay)
            else:
                due = time.monotonic()

    def send_telemetry() -> None:
        match.tick_fast()
        e = match.elapsed
        m = pb.RobotTelemetry(
            sequence=match.seq,
            timestamp_us=int(time.time() * 1e6),
            gimbal_yaw=45 * math.sin(e * 0.6),
            gimbal_pitch=12 * math.sin(e * 0.9),
            chassis_mode=match.chassis_mode,
            vision_online=match.vision_online,
            autoaim_enabled=match.autoaim,
            target_locked=match.target_locked,
            target_id=match.target_id if match.target_locked else 0,
            friction_rpm=6300 + 80 * math.sin(e * 3),
            fire_permit=match.target_locked and match.heat < 240 and match.ammo > 0,
        )
        if match.target_locked:
            m.target_distance = 2.2 + 1.9 * abs(math.sin(e * 0.35))
            m.target_yaw_err = 2.4 * math.sin(e * 2.1)
            m.target_pitch_err = 1.1 * math.sin(e * 1.7)
            m.confidence = 0.72 + 0.26 * abs(math.sin(e * 0.8))
            m.bbox_cx = 960 + 420 * math.sin(e * 0.55)
            m.bbox_cy = 540 + 190 * math.sin(e * 0.42)
            m.bbox_w = 210 - 70 * abs(math.sin(e * 0.35))
            m.bbox_h = 150 - 50 * abs(math.sin(e * 0.35))
        _publish(client, T_TELEMETRY, m)

    def send_game_status() -> None:
        stage, remain = match.stage()
        _publish(client, T_GAME_STATUS, pb.GameStatus(
            current_round=1, total_rounds=3,
            red_score=match.red_score, blue_score=match.blue_score,
            current_stage=stage, stage_countdown_sec=remain,
            stage_elapsed_sec=int(match.elapsed), is_paused=False,
            winner=255, end_reason=255,
        ))

    def send_dynamic() -> None:
        match.tick_10hz()
        _publish(client, T_ROBOT_DYNAMIC, pb.RobotDynamicStatus(
            robot_id=SELF_ROBOT_ID, current_hp=match.hp, max_hp=match.max_hp,
            shooter_heat_17mm=match.heat, shooter_heat_limit=300,
            bullet_speed=29.4 + 0.4 * math.sin(match.elapsed),
            remaining_ammo=match.ammo, coin=match.coin,
            chassis_power=45 + 30 * abs(math.sin(match.elapsed * 1.3)),
            buffer_energy=max(0.0, 60 - 25 * abs(math.sin(match.elapsed * 1.1))),
        ))

    def send_module() -> None:
        rng = match.rng
        _publish(client, T_ROBOT_MODULE, pb.RobotModuleStatus(
            power_manager=1, main_controller=1, armor=1,
            small_shooter=1, big_shooter=0,
            video_transmission=1,
            capacitor=1 if rng.random() > 0.1 else 0,
            uwb=1, rfid=1, light_strip=1,
            laser_detection_module=1 if rng.random() > 0.05 else 2,
        ))

    def send_position() -> None:
        e = match.elapsed
        _publish(client, T_ROBOT_POSITION, pb.RobotPosition(
            x=14.0 + 5.5 * math.sin(e * 0.2),
            y=7.5 + 3.5 * math.cos(e * 0.17),
            yaw=math.degrees(math.sin(e * 0.3)),
        ))

    def send_events() -> None:
        while match.events:
            level, text = match.events.pop(0)
            _publish(client, T_EVENT, pb.Event(
                timestamp_ms=int(time.time() * 1000), level=level, text=text))

    workers = [
        threading.Thread(target=loop, args=(1 / 50, send_telemetry), daemon=True),
        threading.Thread(target=loop, args=(1 / 5, send_game_status), daemon=True),
        threading.Thread(target=loop, args=(1 / 10, send_dynamic), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_module), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_position), daemon=True),
        threading.Thread(target=loop, args=(1 / 20, send_events), daemon=True),
    ]
    for w in workers:
        w.start()

    try:
        while True:
            time.sleep(5)
            stage, remain = match.stage()
            print(f"[server] 阶段={stage} 剩余={remain}s HP={match.hp} 热量={match.heat} "
                  f"弹量={match.ammo} 锁定={'是' if match.target_locked else '否'}")
    except KeyboardInterrupt:
        print("\n[server] 停止")
    finally:
        stop.set()
        client.loop_stop()
        client.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
