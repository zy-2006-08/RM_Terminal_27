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
import os
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
from sim.constants import (
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
    T_ROBOT_HEALTH_SET,
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

# 阵容编号对齐 RMUC 2027 规则手册表 2-1：每方 5 台 = 1 重装 / 2,3 步兵 / 4 空中 / 5 哨兵。
# 编号→兵种的对应见 cpp/presentation.cpp robot_class_name()，两处必须一致，
# 否则终端会对模拟器发出的编号退回显示纯数字。蓝方沿用附录二 +100 偏移。
# 正式协议未发布，这些编号为本地模拟词汇，不得视为正式合同。
# 自机 SELF_ROBOT_ID=3 是红方步兵，故 _FRIENDLY_IDS 不含 3。
_FRIENDLY_IDS: tuple[int, ...] = (1, 2, 4, 5)
_ENEMY_IDS: tuple[int, ...] = (101, 102, 103, 104, 105)

_RED_IDS: tuple[int, ...] = tuple(sorted((*_FRIENDLY_IDS, SELF_ROBOT_ID)))

# 基地 5000 HP 配当前掉血速率要 455s 才打空，比一局 420s 还长，局分永远停在 0:0。
# 1500 HP 约 2.3 分钟一局，演示时才看得到局分变化。
_BASE_MAX_HP = 5000
_OUTPOST_MAX_HP = 1500
# 结算动画要等满一局才看得到,演示/验收时用 RM_MATCH_SEC 缩短比赛。
_MATCH_SEC = int(os.environ.get("RM_MATCH_SEC", "420"))
_SETTLEMENT_SEC = int(os.environ.get("RM_SETTLEMENT_SEC", "15"))
_STAGE_SETTLEMENT = 5
_RED_TEAM_NAME = "电子科技大学中山学院 RoboBraver"
_BLUE_TEAM_NAME = "哈尔滨工业大学(威海) HERO"

_FACTION_RED = 1
_FACTION_BLUE = 2
# 自机阵亡事件必须与队友走同一 faction 来源，否则横幅配色会不一致。
_SELF_FACTION = _FACTION_RED if SELF_ROBOT_ID in _RED_IDS else _FACTION_BLUE

_FIELD_LENGTH_M = 28.0
_FIELD_WIDTH_M = 15.0

# 必须与 cpp/presentation.cpp robot_class_name() 逐字一致，否则事件文本与花名册对同一台车的称呼会不同。
_ROBOT_CLASS_NAMES = {1: "重装", 2: "步兵", 3: "步兵", 4: "空中", 5: "哨兵"}


def _robot_name(robot_id: int) -> str:
    number = robot_id - 100 if robot_id > 100 else robot_id
    faction = "红方" if robot_id in _RED_IDS else "蓝方"
    return f"{faction}{_ROBOT_CLASS_NAMES.get(number, str(number))}"


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
        self._locked_winner: int | None = None
        self.seq = 0
        self.rng = random.Random(2027)
        # 全场血量：自机血量仍由 self.hp 单独维护（既有 RobotDynamicStatus 链路），
        # 这里只存其余机器人，避免同一台机器人有两个互相矛盾的血量来源。
        self.other_hp = {
            robot_id: 400 for robot_id in (*_FRIENDLY_IDS, *_ENEMY_IDS)
        }
        self.other_max_hp = {
            robot_id: 600 for robot_id in (*_FRIENDLY_IDS, *_ENEMY_IDS)
        }
        self.red_base_hp = _BASE_MAX_HP
        self.blue_base_hp = _BASE_MAX_HP
        self.red_outpost_hp = _OUTPOST_MAX_HP
        self.blue_outpost_hp = _OUTPOST_MAX_HP
        self.red_economy = 600
        self.blue_economy = 600
        # 累计总伤害由每次实际扣血累加（见 _deal_damage），不额外造随机数：面板
        # 同时显示血量差和总伤害，两者取自同一次扣血才不会互相矛盾。
        self.red_total_damage = 0
        self.blue_total_damage = 0
        self.red_fortress_sec = 0
        self.blue_fortress_sec = 0
        self.fortress_holder = 0
        self._fortress_accum = 0.0
        # 击杀数同样绑定到真实致死事件（见 _register_kill），不独立造数：
        # 否则会出现击杀数上涨却无人掉血的自相矛盾画面。
        self.red_kills = 0
        self.blue_kills = 0
        self.red_energy_activations = 0
        self.blue_energy_activations = 0
        self.target_locked = False
        self.target_id = 0
        self.vision_online = True
        self.chassis_mode = 2
        self.autoaim = True
        # (level, text, faction)：faction 0未知/中立 1红方 2蓝方，决定事件横幅配色。
        self.events: list[tuple[int, str, int]] = []
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
        """准备 5s -> 倒计时 5s -> 比赛 420s -> 结算 15s，循环往复便于长时间演示。

        结算阶段必须真的出现：终端的结算动画只在 stage==5 且 winner 有效时播放，
        少了这一段就永远验证不到胜负画面。
        """
        e = self.elapsed
        if e < 5:
            return 1, int(5 - e)
        if e < 10:
            return 3, int(10 - e)
        cycle = (e - 10) % (_MATCH_SEC + _SETTLEMENT_SEC)
        if cycle < _MATCH_SEC:
            return 4, int(_MATCH_SEC - cycle)
        return 5, int(_MATCH_SEC + _SETTLEMENT_SEC - cycle)

    def winner(self) -> int:
        """0 平局 1 红胜 2 蓝胜，非结算阶段为 255（协议规定的占位值）。

        进入结算的那一刻按基地血量定胜负并**锁住**结果。基地血量每 tick 仍在变化，
        不锁的话胜方会在结算期间来回翻转，终端那边就会反复重播胜利动画。
        """
        stage, _ = self.stage()
        if stage != _STAGE_SETTLEMENT:
            self._locked_winner = None
            return 255
        if self._locked_winner is None:
            if self.red_base_hp > self.blue_base_hp:
                self._locked_winner = 1
            elif self.blue_base_hp > self.red_base_hp:
                self._locked_winner = 2
            else:
                self._locked_winner = 0
        return self._locked_winner

    def tick_fast(self) -> None:
        """50Hz：更新云台、自瞄与目标状态。"""
        self.seq += 1
        e = self.elapsed

        if self.rng.random() < 0.004:
            self.vision_online = not self.vision_online

        if self.vision_online and self.autoaim:
            if self.rng.random() < 0.02:
                self.target_locked = not self.target_locked
                if self.target_locked:
                    self.target_id = self.rng.choice(_ENEMY_IDS)
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
        if self.hp == 0:
            if self.rng.random() < 0.04:
                self.hp = self.max_hp
                self.ammo = min(400, self.ammo + 100)
            return

        if self.rng.random() < 0.05:
            dmg = self.rng.randint(5, 40)
            applied = min(self.hp, dmg)
            self.hp -= applied
            self._deal_damage(True, applied)
            if self.hp == 0:
                self.events.append((3, f"{_robot_name(SELF_ROBOT_ID)}阵亡", _SELF_FACTION))
        if self.rng.random() < 0.03:
            self.hp = min(self.max_hp, self.hp + 60)
            self.coin += 20
        self.tick_field_hp()
        self.tick_fortress()

    def tick_fortress(self) -> None:
        if self.rng.random() < 0.01:
            self.fortress_holder = self.rng.choice((0, 1, 2))
        if self.fortress_holder == 0:
            return
        # 占领秒数按 10Hz 累加:每 tick 0.1s,攒满 1s 才 +1,否则秒数会以 10 倍速跑。
        self._fortress_accum += 0.1
        if self._fortress_accum < 1.0:
            return
        self._fortress_accum -= 1.0
        if self.fortress_holder == 1:
            self.red_fortress_sec += 1
        else:
            self.blue_fortress_sec += 1

    def _deal_damage(self, victim_is_red: bool, amount: int) -> None:
        if victim_is_red:
            self.blue_total_damage += amount
        else:
            self.red_total_damage += amount

    def _register_kill(self, victim_is_red: bool) -> None:
        if victim_is_red:
            self.blue_kills += 1
        else:
            self.red_kills += 1

    def tick_field_hp(self) -> None:
        """推进除自机外的全场血量与双方基地血量。

        比分改为局分语义：基地被打空即该局结束、对方 +1 局分并重置基地。此前用
        每 tick 1% 概率给 red_score += 1 的累计计数，跑几分钟就攒到几百，和真实
        赛制的 0-3 局分完全不是一个量级。
        """
        for robot_id, hp in self.other_hp.items():
            if hp == 0:
                if self.rng.random() < 0.04:
                    self.other_hp[robot_id] = self.other_max_hp[robot_id]
                continue
            if self.rng.random() < 0.05:
                applied = min(hp, self.rng.randint(5, 40))
                self.other_hp[robot_id] = hp - applied
                self._deal_damage(robot_id in _RED_IDS, applied)
                if self.other_hp[robot_id] == 0:
                    victim_is_red = robot_id in _RED_IDS
                    self._register_kill(victim_is_red)
                    level = 1 if victim_is_red else 0
                    self.events.append(
                        (level, f"{_robot_name(robot_id)}阵亡", 1 if victim_is_red else 2))
            elif self.rng.random() < 0.03:
                self.other_hp[robot_id] = min(
                    self.other_max_hp[robot_id], hp + 60)

        # 按 10Hz 折算:0.02 × 均值 50 × 10 tick ≈ 10 HP/s,1500 血约 150s 倒,比基地略早。
        # `> 0` 是边沿判定,不可省:血量已是 0 时再减仍是 0,只看减后结果会让前哨站倒下后每 tick 重播一次。
        if self.rng.random() < 0.02 and self.blue_outpost_hp > 0:
            applied = min(self.blue_outpost_hp, self.rng.randint(20, 80))
            self.blue_outpost_hp -= applied
            self._deal_damage(False, applied)
            if self.blue_outpost_hp == 0:
                self.events.append((0, "蓝方前哨站被摧毁", 2))
        if self.rng.random() < 0.018 and self.red_outpost_hp > 0:
            applied = min(self.red_outpost_hp, self.rng.randint(20, 80))
            self.red_outpost_hp -= applied
            self._deal_damage(True, applied)
            if self.red_outpost_hp == 0:
                self.events.append((2, "红方前哨站被摧毁", 1))

        if self.rng.random() < 0.02:
            applied = min(self.blue_base_hp, self.rng.randint(20, 90))
            self.blue_base_hp -= applied
            self._deal_damage(False, applied)
        if self.rng.random() < 0.018:
            applied = min(self.red_base_hp, self.rng.randint(20, 90))
            self.red_base_hp -= applied
            self._deal_damage(True, applied)

        # 收入期望需盖过支出期望,否则剩余经济几十秒内就被抽干贴在 0:
        # 收 0.18×27.5≈4.95/tick vs 支 0.05×80≈4.0/tick,净微增并在数百区间震荡。
        if self.rng.random() < 0.18:
            self.red_economy += self.rng.randint(10, 45)
        if self.rng.random() < 0.18:
            self.blue_economy += self.rng.randint(10, 45)
        if self.rng.random() < 0.05:
            self.red_economy = max(0, self.red_economy - self.rng.randint(40, 120))
        if self.rng.random() < 0.05:
            self.blue_economy = max(0, self.blue_economy - self.rng.randint(40, 120))

        if self.blue_base_hp == 0:
            self.red_score += 1
            self.events.append((0, "蓝方基地被击毁，红方本局获胜", 1))
            self.start_next_round()
        elif self.red_base_hp == 0:
            self.blue_score += 1
            self.events.append((3, "红方基地被击毁，蓝方本局获胜", 2))
            self.start_next_round()

    def start_next_round(self) -> None:
        self.red_base_hp = _BASE_MAX_HP
        self.blue_base_hp = _BASE_MAX_HP
        self.red_outpost_hp = _OUTPOST_MAX_HP
        self.blue_outpost_hp = _OUTPOST_MAX_HP
        self.red_economy = 600
        self.blue_economy = 600
        self.red_total_damage = 0
        self.blue_total_damage = 0
        self.red_fortress_sec = 0
        self.blue_fortress_sec = 0
        self.fortress_holder = 0
        self._fortress_accum = 0.0
        self.red_kills = 0
        self.blue_kills = 0
        self.red_energy_activations = 0
        self.blue_energy_activations = 0

    def field_health(self) -> list[tuple[int, int, int, int]]:
        out: list[tuple[int, int, int, int]] = [
            (SELF_ROBOT_ID, _FACTION_RED, self.hp, self.max_hp)
        ]
        for robot_id, hp in self.other_hp.items():
            faction = _FACTION_RED if robot_id in _FRIENDLY_IDS else _FACTION_BLUE
            out.append((robot_id, faction, hp, self.other_max_hp[robot_id]))
        return out

    def match_elapsed(self) -> float | None:
        stage, _ = self.stage()
        if stage != 4:
            return None
        # 周期必须与 stage() 完全一致。写死 420 会与含结算段的真实周期错开,
        # 致盲时刻表随之漂移,表现为 UI 模式在一局内多切一次。
        return (self.elapsed - 10) % (_MATCH_SEC + _SETTLEMENT_SEC)

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
            self.events.append((3, "红方基地被飞镖命中，图传致盲", 1))
            print(f"[server] 致盲开始 match_t={start:.1f}s 持续={duration:.1f}s", flush=True)
        elif active is None and self._blind_window is not None:
            start, duration = self._blind_window
            self.blinded = False
            self.blind_cause = 0
            self.blind_remaining_ms = 0
            self._blind_window = None
            self.events.append((0, "基地致盲结束，图传恢复", 0))
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
            match.events.append((0, f"底盘模式切换为 {param}", 0))
        elif cmd == 0x02:
            match.autoaim = bool(param)
            match.events.append((0, f"自瞄{'启用' if param else '关闭'}", 0))
        elif cmd == 0x03:
            match.chassis_mode = 0
            match.autoaim = False
            match.events.append((3, "终端下发紧急停止", 0))
        elif cmd == 0x04:
            match.events.append((0, "云台回中", 0))

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
            winner=match.winner(), end_reason=255,
            red_base_hp=match.red_base_hp, red_base_max_hp=_BASE_MAX_HP,
            blue_base_hp=match.blue_base_hp, blue_base_max_hp=_BASE_MAX_HP,
            red_outpost_hp=match.red_outpost_hp, red_outpost_max_hp=_OUTPOST_MAX_HP,
            blue_outpost_hp=match.blue_outpost_hp, blue_outpost_max_hp=_OUTPOST_MAX_HP,
            red_team_name=_RED_TEAM_NAME, blue_team_name=_BLUE_TEAM_NAME,
            red_economy=match.red_economy, blue_economy=match.blue_economy,
            red_total_damage=match.red_total_damage,
            blue_total_damage=match.blue_total_damage,
            red_fortress_sec=match.red_fortress_sec,
            blue_fortress_sec=match.blue_fortress_sec,
            fortress_holder=match.fortress_holder,
            red_kills=match.red_kills, blue_kills=match.blue_kills,
            red_energy_activations=match.red_energy_activations,
            blue_energy_activations=match.blue_energy_activations,
        ))

    def send_health_set() -> None:
        m = pb.RobotHealthSet()
        for robot_id, faction, hp, max_hp in match.field_health():
            entry = m.entries.add()
            entry.robot_id = robot_id
            entry.faction = faction
            entry.current_hp = hp
            entry.max_hp = max_hp
        _publish(client, T_ROBOT_HEALTH_SET, m)

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
            level, text, faction = match.events.pop(0)
            _publish(client, T_EVENT, pb.Event(
                timestamp_ms=int(time.time() * 1000), level=level, text=text,
                faction=faction))

    workers = [
        threading.Thread(target=loop, args=(1 / 50, send_telemetry), daemon=True),
        threading.Thread(target=loop, args=(1 / 5, send_game_status), daemon=True),
        threading.Thread(target=loop, args=(1 / 10, send_dynamic), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_module), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_position), daemon=True),
        threading.Thread(target=loop, args=(1 / 5, send_blind_status), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_position_set), daemon=True),
        threading.Thread(target=loop, args=(1 / 5, send_health_set), daemon=True),
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
