"""模拟赛事引擎服务器 + 模拟机器人遥测。

按 2026 协议规定的频率通过 MQTT 发布 Protobuf 消息：
    GameStatus         5Hz
    RobotDynamicStatus 10Hz
    RobotModuleStatus  1Hz
    RobotPosition      1Hz
    Event              触发式
    RobotTelemetry     50Hz（机器人 -> 终端，对应 0x0310）
同时订阅终端下行的 CustomControl / CommonCommand 并打印，验证双向链路。

【本地模拟词汇，非正式协议】RM2027 至今未发布正式协议，其中**没有**任何
「基地致盲」状态字段，也**没有**多机器人位置集合。下列两路数据完全是本文件
编造出来给终端联调用的：
    BlindStatus        5Hz   —— 编造的致盲状态，时刻表见 _BLIND_SCHEDULE
    RobotPositionSet   1Hz   —— 编造的 6 台车位置（自身 + 2 友军 + 3 敌方）
任何人不得把这两个消息的字段当成 2027 正式协议依据。真机接入时必须整段替换。
自身坐标是唯一的例外：它复用既有 RobotPosition 轨迹，不另行编造。
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
    T_BLIND_STATUS,
    T_COMMON_COMMAND,
    T_CUSTOM_CONTROL,
    T_EVENT,
    T_GAME_STATUS,
    T_ROBOT_DYNAMIC,
    T_ROBOT_MODULE,
    T_ROBOT_POSITION,
    T_ROBOT_POSITION_SET,
    T_TELEMETRY,
)

_CMD_NAMES = {
    0x01: "设置底盘模式",
    0x02: "切换自瞄",
    0x03: "紧急停止",
    0x04: "云台回中",
}

# 编造的致盲时刻表（本地模拟，非 2027 协议）。元组为 (比赛内起始秒, 持续秒)，
# 基准是 stage()==4 的比赛内已进行秒数，不是挂钟时间，故两次运行必然一致。
#
# 第一段起点取 30s（= 进程启动后 40s，因为 stage()==4 从挂钟 10s 才开始）是刻意
# 的：致盲 12s + 终端 3s 退出滞回必须在 60s 验收窗口内跑完，否则只能观察到进入
# 而观察不到退出。若把起点改成比赛内 40s（= 启动后 50s），退出会落在 65s，60s
# 的验收就只剩一条切换记录、无法证明滞回真的会放行。
_BLIND_SCHEDULE: tuple[tuple[float, float], ...] = ((30.0, 12.0), (150.0, 12.0))

_BLIND_CAUSE_DART = 1

# 编造的友军 / 敌方阵容（本地模拟，非 2027 协议）。附录二真实 ID 编码未公布，
# 这里只保证「自身之外还有 5 台车、阵营各半」，不声称 ID 与真实赛制对应。
_FRIENDLY_IDS: tuple[int, ...] = (1, 2)
_ENEMY_IDS: tuple[int, ...] = (101, 103, 104)

_FACTION_RED = 1
_FACTION_BLUE = 2

_FIELD_LENGTH_M = 28.0
_FIELD_WIDTH_M = 15.0


def _self_pose(e: float) -> tuple[float, float, float]:
    """自身位姿的唯一函数来源，RobotPosition 与 RobotPositionSet 都必须走它。

    调用方必须传入 _pose_tick() 量化后的时间：两个 sender 跑在各自的 1Hz 线程
    上，若各自直接读 match.elapsed，采样点会相差几十微秒，同一时刻的两条消息
    就会带上毫米级不同的自身坐标，与「同源」的说法不符。
    """
    return (14.0 + 5.5 * math.sin(e * 0.2),
            7.5 + 3.5 * math.cos(e * 0.17),
            math.degrees(math.sin(e * 0.3)))


def _pose_tick(elapsed: float) -> float:
    return math.floor(elapsed * 10.0) / 10.0


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
        self.blinded = False
        self.blind_started_ms = 0
        self.blind_remaining_ms = 0
        self.blind_cause = 0
        self._blind_window: tuple[float, float] | None = None
        self._robot_phases = {
            robot_id: (self.rng.uniform(0, math.tau), self.rng.uniform(0.11, 0.31))
            for robot_id in (*_FRIENDLY_IDS, *_ENEMY_IDS)
        }

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

    def match_elapsed(self) -> float | None:
        stage, _ = self.stage()
        if stage != 4:
            return None
        return (self.elapsed - 10) % 420

    def tick_blind(self) -> None:
        """按 _BLIND_SCHEDULE 推进编造的致盲状态（本地模拟，非 2027 协议）。

        时基是比赛内已进行秒数，不含任何随机数与挂钟读数，因此同一份代码两次
        运行的致盲起止时刻必然逐次相同 —— 这是 todo 7 可复现性验收的依据。
        """
        me = self.match_elapsed()
        active: tuple[float, float] | None = None
        if me is not None:
            for start, duration in _BLIND_SCHEDULE:
                if start <= me < start + duration:
                    active = (start, duration)
                    break

        if active is not None and self._blind_window is None:
            start, duration = active
            self.blinded = True
            self.blind_cause = _BLIND_CAUSE_DART
            # 编造字段：用比赛内 ms 而非挂钟 ms，保证两次运行完全一致。
            self.blind_started_ms = int(start * 1000)
            self._blind_window = active
            self.events.append((3, "我方基地被飞镖命中，图传致盲"))
            print(f"[server] 致盲开始 match_t={start:.1f}s 持续={duration:.1f}s", flush=True)
        elif active is None and self._blind_window is not None:
            start, duration = self._blind_window
            self.blinded = False
            self.blind_cause = 0
            self.blind_remaining_ms = 0
            self._blind_window = None
            self.events.append((0, "基地致盲结束，图传恢复"))
            print(f"[server] 致盲结束 match_t={start + duration:.1f}s", flush=True)

        if self._blind_window is not None and me is not None:
            start, duration = self._blind_window
            self.blind_remaining_ms = max(0, int((start + duration - me) * 1000))

    def fabricated_other_poses(self) -> list[tuple[int, int, float, float, float]]:
        """编造的友军 / 敌方位姿（本地模拟，非 2027 协议）。

        轨迹只依赖构造期由 self.rng 抽定的相位与角速度，加上比赛时基，故可复现。
        不得改用全局 random 或挂钟时间，否则两次运行的地图轨迹不再可比。
        """
        e = self.elapsed
        poses: list[tuple[int, int, float, float, float]] = []
        for robot_id in (*_FRIENDLY_IDS, *_ENEMY_IDS):
            phase, speed = self._robot_phases[robot_id]
            faction = _FACTION_RED if robot_id in _FRIENDLY_IDS else _FACTION_BLUE
            x = _FIELD_LENGTH_M / 2 + (_FIELD_LENGTH_M / 2 - 2.0) * math.sin(e * speed + phase)
            y = _FIELD_WIDTH_M / 2 + (_FIELD_WIDTH_M / 2 - 1.5) * math.cos(e * speed * 0.8 + phase)
            poses.append((robot_id, faction, x, y, math.degrees(math.sin(e * speed + phase))))
        return poses


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
        x, y, yaw = _self_pose(_pose_tick(match.elapsed))
        _publish(client, T_ROBOT_POSITION, pb.RobotPosition(x=x, y=y, yaw=yaw))

    def send_blind_status() -> None:
        match.tick_blind()
        m = pb.BlindStatus(
            self_base_blinded=match.blinded,
            blind_remaining_ms=match.blind_remaining_ms,
            cause=match.blind_cause,
        )
        if match.blinded:
            m.blind_started_ms = match.blind_started_ms
        _publish(client, T_BLIND_STATUS, m)

    def send_position_set() -> None:
        m = pb.RobotPositionSet()
        # 自身坐标必须复用既有 RobotPosition 轨迹，不另行编造：终端侧
        # (todo 6) 会丢弃集合内的自身坐标、只取单机路径的值，两处若不同源
        # 会让地图与数字读数对不上。
        sx, sy, syaw = _self_pose(_pose_tick(match.elapsed))
        own = m.entries.add()
        own.robot_id = SELF_ROBOT_ID
        own.faction = _FACTION_RED
        own.is_self = True
        own.x, own.y, own.yaw = sx, sy, syaw
        for robot_id, faction, x, y, yaw in match.fabricated_other_poses():
            entry = m.entries.add()
            entry.robot_id = robot_id
            entry.faction = faction
            entry.is_self = False
            entry.x, entry.y, entry.yaw = x, y, yaw
        _publish(client, T_ROBOT_POSITION_SET, m)

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
        threading.Thread(target=loop, args=(1 / 5, send_blind_status), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_position_set), daemon=True),
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
