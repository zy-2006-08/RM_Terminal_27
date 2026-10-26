"""模拟赛事引擎服务器 + 模拟机器人遥测。

按 2026 协议规定的频率通过 MQTT 发布 Protobuf 消息：
    GameStatus         5Hz
    RobotDynamicStatus 10Hz
    RobotModuleStatus  1Hz
    RobotPosition      1Hz
    Event              触发式
    RobotTelemetry     50Hz（机器人 -> 终端，对应 0x0310）
同时订阅终端下行的 CustomControl / CommonCommand：CustomControl 载荷是机械臂的
关节角，模拟的车据此「跟随」，0.2 秒收不到就停住，状态发到 sim/ArmEcho。

【本地模拟词汇，非正式协议】RM2027 至今未发布正式协议，其中**没有**任何
「基地致盲」状态字段，也**没有**多机器人位置集合。下列两路数据完全是本文件
编造出来给终端联调用的：
    BlindStatus        5Hz   —— 手动飞镖致盲（RM_BLIND_AUTO=1 才跑时刻表）
    RobotPositionSet   1Hz   —— 编造的 6 台车位置（自身 + 2 友军 + 3 敌方）
任何人不得把这两个消息的字段当成 2027 正式协议依据。真机接入时必须整段替换。
自身坐标是唯一的例外：它复用既有 RobotPosition 轨迹，不另行编造。
"""

from __future__ import annotations

import json
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
from sim import rules
from sim.constants import (
    MQTT_PORT,
    SELF_ROBOT_ID,
    SERVER_HOST,
    T_BLIND_STATUS,
    T_COMMON_COMMAND,
    T_CUSTOM_CONTROL,
    T_EVENT,
    T_GAME_STATUS,
    T_SIM_AUTO_HP,
    T_ROBOT_DYNAMIC,
    T_ROBOT_MODULE,
    T_ROBOT_POSITION,
    T_ROBOT_HEALTH_SET,
    T_ROBOT_POSITION_SET,
    T_ROBOT_RESPAWN,
    T_SIM_ARM_ECHO,
    T_SIM_LINK_CAR,
    T_TELEMETRY,
)
from sim.arm_protocol import decode_joints_deg

# 车上机械臂多久没收到新姿态就算「停住」。控制器固件 25Hz,丢 5 帧。
_ARM_STOP_AFTER_S = 0.2

# 赛程控制指令,走已有的 CommonCommand 下行通道。编号从 0x10 起。
# 【本地模拟词汇】2027 协议里没有「终端控制赛程」这回事,真机上赛程只由裁判系统决定。
CTRL_BEGIN = 0x10
CTRL_HOLD = 0x11
CTRL_RESET = 0x12
CTRL_SEEK = 0x13    # param = 目标赛程秒数
CTRL_SPEED = 0x14   # param = 倍速 x10(20 表示 2.0 倍)
CTRL_SET_HP = 0x15  # param = 机器人ID x1000 + 血量
CTRL_AUTO_HP = 0x16 # param = 0 关闭随机掉血,1 开启
CTRL_SET_ZONE = 0x17 # param = 阵地序号 x10 + 状态(0未控 1红临时 2红完全 3蓝临时 4蓝完全)
CTRL_SET_STRUCTURE_HP = 0x18 # param = 建筑序号 x10000 + 血量
CTRL_SET_FORTRESS = 0x19     # param = 0/1/2 手动指定占领方, 3 恢复随机
CTRL_USE_REINFORCE = 0x1A    # param = 阵营(1红 2蓝), 消耗 1 次加固机会
CTRL_BLIND = 0x1B            # param = 阵营(1红 2蓝) x10 + 飞镖目标类型(1-5)
CTRL_BLIND_CLEAR = 0x1C      # param = 0, 解除手动致盲
CTRL_SHOOT = 0x1D            # param = 口径(17/42) x100 + 弹丸数
CTRL_DART_OPEN = 0x1E        # param = 阵营(1红 2蓝), 在 30s/240s 窗口开闸
CTRL_DART_SHOOT = 0x1F       # param = 阵营(1红 2蓝) x10 + 飞镖目标类型(1-5)
CTRL_PREP = 0x20             # 进入三分钟准备阶段
CTRL_CHECK = 0x21            # 进入十五秒裁判系统自检
CTRL_COUNTDOWN = 0x22        # 进入五秒倒计时
CTRL_RESUME = 0x23           # 从暂停处继续,不跳阶段
CTRL_MODULE = 0x24           # 重装安装模块化 42mm 发射机构(重装专用)
CTRL_BUY = 0x25              # param = 口径 x10000 + 发数,非远程兑换允许发弹量
CTRL_PAY_REVIVE = 0x26       # 金币兑换立即复活(无参数)
CTRL_CARD = 0x27             # param = 机器人 ID(0=自机),模拟检测到场地交互卡
_CTRL_NAMES = {
    CTRL_BEGIN: "开赛",
    CTRL_HOLD: "暂停",
    CTRL_RESET: "复位到准备阶段",
    CTRL_SEEK: "跳转赛程",
    CTRL_SPEED: "设置倍速",
    CTRL_SET_HP: "设置机器人血量",
    CTRL_AUTO_HP: "随机掉血开关",
    CTRL_SET_ZONE: "设置阵地状态",
    CTRL_SET_STRUCTURE_HP: "设置建筑血量",
    CTRL_SET_FORTRESS: "设置堡垒占领方",
    CTRL_USE_REINFORCE: "消耗加固机会",
    CTRL_BLIND: "手动触发飞镖致盲",
    CTRL_BLIND_CLEAR: "解除手动致盲",
    CTRL_SHOOT: "对锁定目标射击",
    CTRL_DART_OPEN: "开启飞镖闸门",
    CTRL_DART_SHOOT: "发射飞镖",
    CTRL_PREP: "进入准备阶段",
    CTRL_CHECK: "进入裁判系统自检",
    CTRL_COUNTDOWN: "进入五秒倒计时",
    CTRL_RESUME: "继续",
    CTRL_MODULE: "安装 42mm 模块",
    CTRL_BUY: "兑换允许发弹量",
    CTRL_PAY_REVIVE: "金币兑换立即复活",
    CTRL_CARD: "模拟检测到场地交互卡",
}

# 阵地顺序与 GameStatus 的四个字段一一对应,写死顺序是因为控制指令按序号寻址。
_ZONE_NAMES = ("红方前哨区", "蓝方前哨区", "红方基地区", "蓝方基地区")
_ZONE_STATE_NAMES = ("未控", "红方临时", "红方完全", "蓝方临时", "蓝方完全")
_ZONE_RED_TEMP = 1
_ZONE_RED_FULL = 2
_ZONE_BLUE_TEMP = 3
_ZONE_BLUE_FULL = 4
# 规则 5.6.2：控制全部 2 个前哨阵地 +1/秒,控制全部 2 个基地阵地 +3/秒。
# "控制"含临时控制（用户确认）；基地阵地只有初始拥有方能完全控制。
_CONTROL_OUTPOST_RATE = 1
_CONTROL_BASE_RATE = 3
# 规则 5.6.2：比赛开始时双方均完全控制己方的基地阵地与前哨阵地。
_INITIAL_ZONES = (_ZONE_RED_FULL, _ZONE_BLUE_FULL,
                  _ZONE_RED_FULL, _ZONE_BLUE_FULL)
# 红方基地只能红方完全控制或蓝方临时控制,蓝方基地反之。
# 前哨阵地只支持临时/完全控制,不存在「未控」。
_ZONE_ALLOWED_STATES = (
    (_ZONE_RED_TEMP, _ZONE_RED_FULL, _ZONE_BLUE_TEMP, _ZONE_BLUE_FULL),
    (_ZONE_RED_TEMP, _ZONE_RED_FULL, _ZONE_BLUE_TEMP, _ZONE_BLUE_FULL),
    (_ZONE_RED_FULL, _ZONE_BLUE_TEMP),
    (_ZONE_BLUE_FULL, _ZONE_RED_TEMP),
)
# 加固机会每满一分钟双方各 +1。
_REINFORCE_PERIOD_SEC = 60.0
# 表 5-21：连续激活对方完全控制的阵地 5s 可取得临时控制；临时控制方 5s 不再
# 激活就退回原方完全控制。临时控制转完全控制只能靠击毁阵地上的前哨站，持续
# 激活本身不升级。本地模拟没有增益点输入，每次把阵地设成临时控制即视为「此刻
# 仍在激活」，重新下发就把倒计时拉回 5s；停止下发则按比赛秒倒退到 0。
_ZONE_PROGRESS_SEC = 5.0

# 编造的致盲时刻表（本地模拟，非 2027 协议）。元组为 (比赛内起始秒, 持续秒)，
# 基准是 stage()==4 的比赛内已进行秒数，不是挂钟时间，故两次运行必然一致。
#
# 第一段起点取 30s 是刻意的：致盲 12s + 终端 3s 退出滞回必须落在验收窗口内，
# 否则只能观察到进入而观察不到退出。验收脚本把准备/自检/倒计时都压到 1s，
# 比赛从挂钟 3s 开始，于是进入/退出分别在 +33s/+48s 左右。
_BLIND_SCHEDULE: tuple[tuple[float, float], ...] = ((30.0, 12.0), (150.0, 12.0))
# 默认只认手动中镖；模式回归脚本用 RM_BLIND_AUTO=1 打开时刻表。
_BLIND_AUTO = os.environ.get("RM_BLIND_AUTO", "0") == "1"

_BLIND_CAUSE_DART = 1

# 阵容编号对齐 RMUC 2027 规则手册表 3-1：1 重装 / 3,4 步兵 / 6 空中 / 7 哨兵。
# 编号→兵种的对应见 cpp/presentation.cpp robot_class_name()，两处必须一致，
# 否则终端会对模拟器发出的编号退回显示纯数字。蓝方沿用附录二 +100 偏移。
# 正式协议未发布，这些编号为本地模拟词汇，不得视为正式合同。
_FACTION_RED = 1
_FACTION_BLUE = 2
_RED_LINEUP: tuple[int, ...] = (1, 3, 4, 6, 7)
_BLUE_LINEUP: tuple[int, ...] = (101, 103, 104, 106, 107)
if SELF_ROBOT_ID in _RED_LINEUP:
    _SELF_FACTION = _FACTION_RED
    _FRIENDLY_IDS: tuple[int, ...] = tuple(i for i in _RED_LINEUP if i != SELF_ROBOT_ID)
    _ENEMY_IDS: tuple[int, ...] = _BLUE_LINEUP
elif SELF_ROBOT_ID in _BLUE_LINEUP:
    _SELF_FACTION = _FACTION_BLUE
    _FRIENDLY_IDS = tuple(i for i in _BLUE_LINEUP if i != SELF_ROBOT_ID)
    _ENEMY_IDS = _RED_LINEUP
else:
    raise ValueError(f"RM_SELF_ID={SELF_ROBOT_ID} 不在 2027 阵容 {_RED_LINEUP + _BLUE_LINEUP} 内")
_RED_IDS: tuple[int, ...] = _RED_LINEUP
_SELF_LOCAL_ID = SELF_ROBOT_ID - 100 if SELF_ROBOT_ID > 100 else SELF_ROBOT_ID

# 表 5-16/5-17/5-18：重装 400,步兵 200,空中 200,哨兵按自动哨兵 400。
_ROBOT_MAX_HP = {1: 400, 3: 200, 4: 200, 6: 200, 7: 400}

# 规则 5.6.1：基地 10000 HP,前哨站 1000 HP,击毁后重建 500 HP。
_BASE_MAX_HP = 10000
_OUTPOST_MAX_HP = 1000
_OUTPOST_REBUILD_HP = 500
# 建筑控制指令的寻址顺序，与 _STRUCTURE_MAX_HP 一一对应。
_STRUCTURE_NAMES = ("红方基地", "蓝方基地", "红方前哨站", "蓝方前哨站")
_STRUCTURE_MAX_HP = (_BASE_MAX_HP, _BASE_MAX_HP, _OUTPOST_MAX_HP, _OUTPOST_MAX_HP)

# 飞镖目标类型与遮挡时长，取自规则 5.7.3：固定类目标第 1~4 次命中分别遮挡
# 10/5/3/2 秒，随机移动类每发都遮挡 10 秒。
_DART_TARGET_NAMES = ("前哨站", "基地固定目标", "基地随机固定目标",
                      "基地随机移动目标", "基地末端移动目标")
_DART_HIT_DURATIONS = (10.0, 5.0, 3.0, 2.0)
# 规则 5.7.3：每局 4 枚；30s/240s 各 1 次开闸机会；开闸 7s，发射 30s，冷却 15s；
# 同一目标两次命中至少间隔 2s。命中伤害与移动类目标对地面机器人的扣血比例见下表。
_DART_MAX_PER_ROUND = 4
_DART_UNLOCK_SEC = (30.0, 240.0)
_DART_GATE_OPEN_SEC = 7.0
_DART_FIRE_WINDOW_SEC = 30.0
_DART_COOLDOWN_SEC = 15.0
_DART_TARGET_COOLDOWN_SEC = 2.0
_DART_DAMAGE = (1000, 200, 300, 800, 1250)
_DART_ROBOT_HP_FRACTION = (0.0, 0.0, 0.0, 0.20, 0.50)
# 规则 5.1：17mm 单发 20,42mm 单发 200；规则 5.1.3：单发热量 +10/+100。
_PELLET_DAMAGE = {17: 20, 42: 200}
_PELLET_HEAT = {17: 10, 42: 100}
# 规则 5.4.1：开局 800 金币,05:59~00:59 每整分钟 +100,最后一分钟 +300。
_INITIAL_ECONOMY = 800
_ECONOMY_MILESTONES = ((61, 100), (121, 100), (181, 100),
                       (241, 100), (301, 100), (361, 300))
# 规则 5.2：当局累计命中弹丸数≥10 才能消耗加固机会。
_REINFORCE_MIN_HITS = 10
# 结算动画要等满一局才看得到,演示/验收时用 RM_MATCH_SEC 缩短比赛。
_MATCH_SEC = int(os.environ.get("RM_MATCH_SEC", "420"))
_SETTLEMENT_SEC = int(os.environ.get("RM_SETTLEMENT_SEC", "15"))
# 规则 6：3 分钟准备 + 15 秒自检 + 5 秒倒计时 = 200 秒,不属于比赛时间。
# 终端联调不必真等,脚本用 RM_PREP_SEC/RM_CHECK_SEC/RM_COUNTDOWN_SEC 缩短。
_PREP_SEC = int(os.environ.get("RM_PREP_SEC", "180"))
_CHECK_SEC = int(os.environ.get("RM_CHECK_SEC", "15"))
_COUNTDOWN_SEC = int(os.environ.get("RM_COUNTDOWN_SEC", "5"))
# 默认自动流转:启动即从赛外进入准备阶段。RM_HOLD=1 才停在赛外,由按钮推进。
_HOLD_AT_START = os.environ.get("RM_HOLD", "0") == "1"
_STAGE_IDLE = 0
_STAGE_PREP = 1
_STAGE_CHECK = 2
_STAGE_COUNTDOWN = 3
_STAGE_MATCH = 4
_STAGE_SETTLEMENT = 5
_RED_TEAM_NAME = "电子科技大学中山学院 RoboBraver"
_BLUE_TEAM_NAME = "哈尔滨工业大学(威海) HERO"

_FIELD_LENGTH_M = 28.0
_FIELD_WIDTH_M = 15.0

# 必须与 cpp/presentation.cpp robot_class_name() 逐字一致，否则事件文本与花名册对同一台车的称呼会不同。
_ROBOT_CLASS_NAMES = {1: "重装", 3: "步兵", 4: "步兵", 6: "空中", 7: "哨兵"}


def _new_dart_state() -> dict:
    return {"used": 0, "open_at": -1.0, "close_at": -1.0, "cooldown_until": -1.0,
            "hits": 0, "fixed_hits": 0, "chances": set(), "last_hit": {}}


def _new_respawn_state() -> dict:
    return {"dead": False, "progress": 0.0, "total": 0, "paid": 0,
            "weak": False, "weak_until": None, "invincible_until": 0.0,
            "revived_at": 0.0, "revived_paid": False}


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
        self.speed = 1.0
        # 默认全手动:血量只由 set_hp/set_structure_hp 改,不会自己掉。
        # 想恢复自动掉血用 `auto 1`(面板上的开关)。
        self.auto_hp = False
        # 待命档:为真时 elapsed 冻结在 hold_at,由操作手决定何时推进。
        self.holding = _HOLD_AT_START
        self.hold_at = 0.0
        self.max_hp = _ROBOT_MAX_HP[_SELF_LOCAL_ID]
        self.hp = self.max_hp
        self.heat = 0.0
        # Q1 超过 Q0 后发射机构锁定，直到 Q1 冷却到 0；Q1 ≥ Q2 则当局永久锁定。
        self.heat_locked = False
        self.heat_permanent_lock = False
        self._heat_last = 0.0
        # 表 5-16/5-17/5-18、5.4.2：自机默认步兵「血量优先 + 冷却优先 1 级」，
        # 允许发弹量为 0（预装弹不等于允许发弹量）；哨兵按自动哨兵；重装未装
        # 模块化 42mm 发射机构前没有发射机构。
        if _SELF_LOCAL_ID == 1:
            self.caliber = 42
            self.heat_limit = rules.HEAVY_HEAT_LIMIT
            self.cooling_rate = rules.HEAVY_COOLING
            self.launcher_ready = False
            self.ammo = 0
        elif _SELF_LOCAL_ID == 7:
            self.caliber = 17
            self.heat_limit, self.cooling_rate = rules.SENTRY_AUTO_HEAT
            self.launcher_ready = True
            self.ammo = rules.SENTRY_INITIAL_AMMO
        elif _SELF_LOCAL_ID == 6:
            # 表 3-7/3-8：空中机器人靠冲击攻击，不带地面发射机构。
            self.caliber = 17
            self.heat_limit, self.cooling_rate = rules.LAUNCHER_COOLING[0]
            self.launcher_ready = False
            self.ammo = 0
        else:
            self.caliber = 17
            self.heat_limit, self.cooling_rate = rules.LAUNCHER_COOLING[0]
            self.launcher_ready = True
            self.ammo = 0
        self.team_ammo_bought = {17: 0, 42: 0}
        self.red_score = 0
        self.blue_score = 0
        self._locked_winner: int | None = None
        # 局末状态:提前击毁基地时把 round_len 截到实际用时,stage() 立即进结算。
        self.round_index = 0
        self.round_start_e = 0.0
        self.round_len = float(_MATCH_SEC)
        self.stage_id = _STAGE_IDLE if _HOLD_AT_START else _STAGE_PREP
        self.stage_start_e = 0.0
        self.round_end_reason = 0
        self.round_winner: int | None = None
        self.match_over = False
        self._econ_paid = 0
        self._round_lock = threading.RLock()
        self.seq = 0
        self.rng = random.Random(2027)
        # 全场血量：自机血量仍由 self.hp 单独维护（既有 RobotDynamicStatus 链路），
        # 这里只存其余机器人，避免同一台机器人有两个互相矛盾的血量来源。
        self.other_hp = {
            robot_id: _ROBOT_MAX_HP[robot_id - 100 if robot_id > 100 else robot_id]
            for robot_id in (*_FRIENDLY_IDS, *_ENEMY_IDS)
        }
        self.other_max_hp = dict(self.other_hp)
        self.level = 1
        self.respawn_state = {
            robot_id: _new_respawn_state()
            for robot_id in (SELF_ROBOT_ID, *self.other_hp)
        }
        self._respawn_last = 0.0
        self.red_base_hp = _BASE_MAX_HP
        self.blue_base_hp = _BASE_MAX_HP
        self.red_outpost_hp = _OUTPOST_MAX_HP
        self.blue_outpost_hp = _OUTPOST_MAX_HP
        self.red_economy = _INITIAL_ECONOMY
        self.blue_economy = _INITIAL_ECONOMY
        # 累计总伤害由每次实际扣血累加（见 _deal_damage），不额外造随机数：面板
        # 同时显示血量差和总伤害，两者取自同一次扣血才不会互相矛盾。
        self.red_total_damage = 0
        self.blue_total_damage = 0
        self.red_fortress_sec = 0
        self.blue_fortress_sec = 0
        self.fortress_holder = 0
        # 默认不随机换主;`fortress auto` 才恢复随机换主。
        self.fortress_auto = False
        self._fortress_accum = 0.0
        # 4 个阵地按 _ZONE_NAMES 的顺序存放状态。控制度和加固按比赛秒结算,
        # 不能按 tick 计数：5 倍速下每个 tick 走 0.5 秒，规则会被悄悄放大。
        # 规则 5.6.2：开局双方完全控制己方前哨阵地和基地阵地。
        self.zones = list(_INITIAL_ZONES)
        # 每个阵地临时控制的剩余激活秒数；完全控制恒为 0。
        self.zone_progress = [0.0] * len(self.zones)
        self.red_control_points = 0
        self.blue_control_points = 0
        self.red_reinforce_chances = 0
        self.blue_reinforce_chances = 0
        self._zone_accum = 0.0
        self._reinforce_accum = 0.0
        self._zone_last = 0.0
        # 击杀数同样绑定到真实致死事件（见 _register_kill），不独立造数：
        # 否则会出现击杀数上涨却无人掉血的自相矛盾画面。
        self.red_kills = 0
        self.blue_kills = 0
        self.red_energy_activations = 0
        self.blue_energy_activations = 0
        # 规则 5.2：加固机会的消耗门槛是「当局累计命中弹丸数≥10」，每次实际
        # 扣血算一发命中，与总伤害同源，不另造随机数。
        self.red_hits = 0
        self.blue_hits = 0
        self.target_locked = False
        self.target_id = 0
        self.vision_online = True
        self.chassis_mode = 2
        self.autoaim = True
        # (level, text, faction)：faction 0未知/中立 1红方 2蓝方，决定事件横幅配色。
        self.events: list[tuple[int, str, int]] = []
        # 致盲按「哪一方被遮挡」建模：0未致盲 1红方 2蓝方。终端只消费
        # self_base_blinded（红方），蓝方中镖用于模拟器和面板联调。
        self.blind_side = 0
        self.blind_target_type = 0
        self.blind_started_ms = 0
        self.blind_remaining_ms = 0
        self.blind_cause = 0
        self._blind_window: tuple[float, float, int, int] | None = None
        self.blind_started = {_FACTION_RED: -1.0, _FACTION_BLUE: -1.0}
        self.blind_until = {_FACTION_RED: -1.0, _FACTION_BLUE: -1.0}
        self.blind_type = {_FACTION_RED: 1, _FACTION_BLUE: 1}
        self.dart_hits = {_FACTION_RED: 0, _FACTION_BLUE: 0}
        self.dart = {side: _new_dart_state() for side in (_FACTION_RED, _FACTION_BLUE)}
        self._robot_phases = {
            robot_id: (self.rng.uniform(0, math.tau), self.rng.uniform(0.11, 0.31))
            for robot_id in (*_FRIENDLY_IDS, *_ENEMY_IDS)
        }

    @property
    def elapsed(self) -> float:
        if self.holding:
            return self.hold_at
        return self.hold_at + (time.monotonic() - self.t0) * self.speed

    def _stage_duration(self, stage_id: int) -> float:
        return {
            _STAGE_PREP: float(_PREP_SEC),
            _STAGE_CHECK: float(_CHECK_SEC),
            _STAGE_COUNTDOWN: float(_COUNTDOWN_SEC),
            _STAGE_MATCH: self.round_len,
            _STAGE_SETTLEMENT: float(_SETTLEMENT_SEC),
        }[stage_id]

    def _enter_stage(self, stage_id: int, start: float) -> None:
        self.stage_id = stage_id
        self.stage_start_e = start

    def _enter_match(self, start: float) -> None:
        self.stage_id = _STAGE_MATCH
        self.stage_start_e = start
        self.round_start_e = start
        self._zone_last = start
        self._zone_accum = 0.0

    def _jump_to(self, stage_id: int) -> None:
        self.hold_at = self.elapsed
        self.t0 = time.monotonic()
        self.holding = False
        self._enter_stage(stage_id, self.hold_at)

    def prep(self) -> None:
        """从当前时刻进入 3 分钟准备阶段。"""
        self._jump_to(_STAGE_PREP)

    def check(self) -> None:
        """从当前时刻进入 15 秒裁判系统自检。"""
        self._jump_to(_STAGE_CHECK)

    def countdown(self) -> None:
        """从当前时刻进入 5 秒倒计时。"""
        self._jump_to(_STAGE_COUNTDOWN)

    def begin(self) -> None:
        """继续赛程；停在赛外时按开赛处理，直接进入准备阶段。"""
        if self.stage_id == _STAGE_IDLE:
            self.prep()
            return
        if not self.holding:
            return
        self.hold_at = self.elapsed
        self.t0 = time.monotonic()
        self.holding = False

    def hold(self) -> None:
        if self.holding:
            return
        self.hold_at = self.elapsed
        self.holding = True

    def reset(self) -> None:
        """复位到赛外，清空局分、局状态和所有场景数据。"""
        self.__init__()
        self.holding = True
        self.hold_at = 0.0
        self.stage_id = _STAGE_IDLE
        self.stage_start_e = 0.0

    def seek(self, target: float) -> None:
        """跳到全局赛程指定秒。比赛内按局内起点推导所在阶段。"""
        target = max(0.0, target)
        self.hold_at = target
        self.t0 = time.monotonic()
        self._locked_winner = None
        if self.stage_id in (_STAGE_MATCH, _STAGE_SETTLEMENT):
            start = self.round_start_e
            self.stage_start_e = start
            self.stage_id = (_STAGE_MATCH if target < start + self.round_len
                             else _STAGE_SETTLEMENT)
            return
        if target < _PREP_SEC:
            self._enter_stage(_STAGE_PREP, 0.0)
        elif target < _PREP_SEC + _CHECK_SEC:
            self._enter_stage(_STAGE_CHECK, float(_PREP_SEC))
        elif target < _PREP_SEC + _CHECK_SEC + _COUNTDOWN_SEC:
            self._enter_stage(_STAGE_COUNTDOWN, float(_PREP_SEC + _CHECK_SEC))
        else:
            self._enter_match(float(_PREP_SEC + _CHECK_SEC + _COUNTDOWN_SEC))

    def set_speed(self, speed: float) -> None:
        """换倍速:先把当前赛程值结算到 hold_at,避免改速瞬间跳变。"""
        self.hold_at = self.elapsed
        self.t0 = time.monotonic()
        self.speed = max(0.1, speed)

    def set_hp(self, robot_id: int, hp: int) -> bool:
        """手动改血量。返回 False 表示没有这台机器人。"""
        if robot_id != SELF_ROBOT_ID and robot_id not in self.other_hp:
            return False
        self._write_hp(robot_id, hp)
        self._sync_respawn(robot_id)
        return True

    def _robot_hp(self, robot_id: int) -> int:
        return self.hp if robot_id == SELF_ROBOT_ID else self.other_hp[robot_id]

    def _write_hp(self, robot_id: int, hp: int) -> None:
        """只写血量，不触发死亡/复活判定；判定统一走 _sync_respawn。"""
        if robot_id == SELF_ROBOT_ID:
            self.hp = max(0, min(self.max_hp, int(hp)))
        else:
            self.other_hp[robot_id] = max(0, min(self.other_max_hp[robot_id], int(hp)))

    @staticmethod
    def _is_ground(robot_id: int) -> bool:
        return robot_id % 100 != 6

    def _friendly_base_hp(self, robot_id: int) -> int:
        return self.red_base_hp if robot_id in _RED_IDS else self.blue_base_hp

    def _sync_respawn(self, robot_id: int) -> None:
        """血量变 0 时开始读条；被手动/复活写成正值时清掉读条。"""
        state = self.respawn_state.get(robot_id)
        if state is None or not self._is_ground(robot_id):
            return
        if self._robot_hp(robot_id) == 0:
            if not state["dead"]:
                self._start_respawn(robot_id)
        else:
            state["dead"] = False
            state["progress"] = 0.0
            state["total"] = 0

    def _start_respawn(self, robot_id: int) -> None:
        state = self.respawn_state[robot_id]
        now = self.match_elapsed() or 0.0
        remaining = max(0.0, rules.MATCH_SEC - now)
        lam = rules.logistics_lambda(self._friendly_base_hp(robot_id))
        state.update(dead=True, progress=0.0, weak=False, weak_until=None,
                     invincible_until=0.0, revived_at=0.0, revived_paid=False)
        state["total"] = rules.revive_delay(remaining, state["paid"], lam)
        if robot_id == SELF_ROBOT_ID:
            # 规则 5.1.3/5.3.2：死亡清热量和临时锁，永久过热锁仍随本局。
            self.heat = 0.0
            self.heat_locked = False

    def _revive(self, robot_id: int, paid: bool) -> None:
        state = self.respawn_state[robot_id]
        now = self.match_elapsed() or 0.0
        fraction = rules.PAY_REVIVE_HP_FRACTION if paid else rules.REVIVE_HP_FRACTION
        maximum = self.max_hp if robot_id == SELF_ROBOT_ID else self.other_max_hp[robot_id]
        self._write_hp(robot_id, max(1, rules.round_half_up(maximum * fraction)))
        duration = rules.PAY_REVIVE_WEAK_SEC if paid else rules.REVIVE_INVINCIBLE_SEC
        state.update(dead=False, progress=0.0, total=0, weak=True,
                     weak_until=now + duration, invincible_until=now + duration,
                     revived_at=now, revived_paid=paid)
        self.events.append((1 if robot_id in _RED_IDS else 0,
                            f"{_robot_name(robot_id)}{'兑换' if paid else ''}复活",
                            1 if robot_id in _RED_IDS else 2))

    def is_weak(self, robot_id: int) -> bool:
        state = self.respawn_state.get(robot_id)
        if state is None:
            return False
        now = self.match_elapsed() or 0.0
        if state["weak"] and state["weak_until"] is not None and now >= state["weak_until"]:
            state["weak"] = False
            state["weak_until"] = None
        return state["weak"]

    def is_invincible(self, robot_id: int) -> bool:
        state = self.respawn_state.get(robot_id)
        return state is not None and (self.match_elapsed() or 0.0) < state["invincible_until"]

    def tick_respawn(self) -> None:
        """按比赛秒推进复活读条；暂停或倍速由 elapsed 统一处理。"""
        now = self.elapsed
        delta = min(max(now - self._respawn_last, 0.0), 1.0)
        self._respawn_last = now
        me = self.match_elapsed()
        for robot_id, state in self.respawn_state.items():
            if not self._is_ground(robot_id):
                continue
            self._sync_respawn(robot_id)
            if not state["dead"] or me is None:
                continue
            rate = rules.revive_rate(False, self._friendly_base_hp(robot_id))
            state["progress"] += rate * delta
            if state["progress"] >= state["total"]:
                self._revive(robot_id, paid=False)

    def pay_revive(self) -> str | None:
        """金币兑换立即复活（本机）。成功返回 None，失败返回原因。"""
        me = self.match_elapsed()
        if me is None:
            return "比赛未开始或已结束"
        state = self.respawn_state.get(SELF_ROBOT_ID)
        if state is None or not self._is_ground(SELF_ROBOT_ID):
            return "该机器人不适用复活"
        if not state["dead"]:
            return "机器人未阵亡"
        remaining = max(0.0, rules.MATCH_SEC - me)
        cost = rules.pay_revive_cost(remaining, self.level)
        if self.coin < cost:
            return f"金币不足，需要 {cost}"
        self._spend_coins(cost)
        state["paid"] += 1
        self._revive(SELF_ROBOT_ID, paid=True)
        return None

    def card_scan(self, robot_id: int = 0) -> str | None:
        """模拟场地交互卡：解除虚弱，免费复活的无敌可缩短到至少 10s。"""
        rid = SELF_ROBOT_ID if robot_id == 0 else robot_id
        state = self.respawn_state.get(rid)
        if state is None or not self._is_ground(rid):
            return "未知或非地面机器人"
        if self.match_elapsed() is None:
            return "比赛未开始或已结束"
        if self._robot_hp(rid) == 0:
            return "机器人已阵亡"
        if not self.is_weak(rid):
            return "机器人未处于虚弱状态"
        state["weak"] = False
        state["weak_until"] = None
        if not state["revived_paid"]:
            state["invincible_until"] = max(
                state["revived_at"] + rules.REVIVE_WEAK_INVINCIBLE_FLOOR_SEC,
                self.match_elapsed() or 0.0)
        return None

    def respawn_status(self) -> tuple[bool, int, int, bool, int, bool]:
        """RobotRespawnStatus 字段；免费复活到点自动确认，故 can_free 只在读条完成瞬间为真。"""
        me = self.match_elapsed()
        state = self.respawn_state.get(SELF_ROBOT_ID)
        if state is None or me is None or not self._is_ground(SELF_ROBOT_ID):
            return False, 0, 0, False, 0, False
        if not state["dead"]:
            return False, 0, 0, False, 0, False
        total = max(0, int(state["total"]))
        current = min(total, int(state["progress"]))
        free = state["progress"] >= state["total"]
        cost = rules.pay_revive_cost(max(0.0, rules.MATCH_SEC - me), self.level)
        return True, total, current, free, cost, cost <= self.coin

    def set_zone(self, index: int, state: int) -> bool:
        """手动设置阵地状态。返回 False 表示阵地序号或状态值非法。"""
        if not 0 <= index < len(self.zones) or not 0 <= state < len(_ZONE_STATE_NAMES):
            return False
        if state not in _ZONE_ALLOWED_STATES[index]:
            return False
        if state in (_ZONE_RED_TEMP, _ZONE_BLUE_TEMP):
            own_full = _ZONE_RED_FULL if state == _ZONE_RED_TEMP else _ZONE_BLUE_FULL
            if self.zones[index] == own_full:
                return True
            self.zones[index] = state
            self.zone_progress[index] = _ZONE_PROGRESS_SEC
            return True
        if index == 0 and self.red_outpost_hp == 0:
            self.red_outpost_hp = _OUTPOST_REBUILD_HP
        elif index == 1 and self.blue_outpost_hp == 0:
            self.blue_outpost_hp = _OUTPOST_REBUILD_HP
        self.zones[index] = state
        self.zone_progress[index] = 0.0
        return True

    def _destroy_outpost(self, red_outpost: bool, attacker: int) -> None:
        """前哨站被击毁：按表 5-21 转化阵地控制，按 5.6.1 重建。

        完全控制方被击毁后降为临时控制；5s 内不继续激活，击毁方转为完全控制
        并把前哨站重建为 500 HP。击毁方本就持有临时控制时直接转为完全控制。
        """
        index = 0 if red_outpost else 1
        state = self.zones[index]
        owner = _FACTION_RED if state in (_ZONE_RED_TEMP, _ZONE_RED_FULL) else _FACTION_BLUE
        name = "红方" if red_outpost else "蓝方"
        owner_name = "红方" if owner == _FACTION_RED else "蓝方"
        if owner == attacker:
            # 击毁方已控制该阵地（完全或临时）时直接转为完全控制并重建，
            # 包括红方已完全控制蓝方前哨阵地后再次击毁的情况。
            self.zones[index] = _ZONE_RED_FULL if owner == _FACTION_RED else _ZONE_BLUE_FULL
            self.zone_progress[index] = 0.0
            if red_outpost:
                self.red_outpost_hp = _OUTPOST_REBUILD_HP
            else:
                self.blue_outpost_hp = _OUTPOST_REBUILD_HP
            self.events.append((2 if red_outpost else 0,
                                f"{name}前哨站被摧毁，"
                                f"{'红方' if owner == _FACTION_RED else '蓝方'}完全控制并重建",
                                owner))
        else:
            if state in (_ZONE_RED_FULL, _ZONE_BLUE_FULL):
                self.zones[index] = (
                    _ZONE_RED_TEMP if owner == _FACTION_RED else _ZONE_BLUE_TEMP)
                text = f"{name}前哨站被摧毁，{owner_name}转为临时控制"
            else:
                text = f"{name}前哨站被摧毁，等待控制权结算"
            self.zone_progress[index] = _ZONE_PROGRESS_SEC
            if red_outpost:
                self.red_outpost_hp = 0
            else:
                self.blue_outpost_hp = 0
            self.events.append((2 if red_outpost else 0, text, owner))

    def set_structure_hp(self, index: int, hp: int) -> bool:
        """手动设置基地/前哨站血量。返回 False 表示建筑序号非法。

        前哨站设为 0 必须走击毁流程,否则只改数字会让它永远停在 0,
        既不转化阵地也不重建——面板上看起来就是「打爆了但没反应」。
        """
        if not 0 <= index < len(_STRUCTURE_NAMES):
            return False
        hp = max(0, min(_STRUCTURE_MAX_HP[index], hp))
        if index == 0:
            self.red_base_hp = hp
        elif index == 1:
            self.blue_base_hp = hp
        elif index == 2:
            if hp == 0 and self.red_outpost_hp > 0:
                self._destroy_outpost(True, _FACTION_BLUE)
            else:
                self.red_outpost_hp = hp
        else:
            if hp == 0 and self.blue_outpost_hp > 0:
                self._destroy_outpost(False, _FACTION_RED)
            else:
                self.blue_outpost_hp = hp
        return True

    def set_fortress(self, holder: int) -> bool:
        """手动指定堡垒占领方并停掉随机换主；3 表示恢复随机。"""
        if holder == 3:
            self.fortress_auto = True
            return True
        if holder not in (0, 1, 2):
            return False
        self.fortress_auto = False
        self.fortress_holder = holder
        return True

    def use_reinforce(self, side: int) -> str | None:
        """按规则 5.2 消耗一次加固机会。成功返回 None，失败返回原因。"""
        if side not in (_FACTION_RED, _FACTION_BLUE):
            return "阵营必须是 1(红) 或 2(蓝)"
        hits = self.red_hits if side == _FACTION_RED else self.blue_hits
        chances = (self.red_reinforce_chances if side == _FACTION_RED
                   else self.blue_reinforce_chances)
        if hits < _REINFORCE_MIN_HITS:
            return f"累计命中 {hits} 发，不足 {_REINFORCE_MIN_HITS} 发"
        if chances <= 0:
            return "没有可用的加固机会"
        if side == _FACTION_RED:
            self.red_reinforce_chances -= 1
        else:
            self.blue_reinforce_chances -= 1
        faction = "红方" if side == _FACTION_RED else "蓝方"
        self.events.append((0, f"{faction}消耗 1 次加固机会", side))
        return None

    def trigger_blind(self, side: int, target_type: int) -> bool:
        """手动触发飞镖致盲。返回 False 表示阵营或目标类型非法。"""
        if side not in (_FACTION_RED, _FACTION_BLUE):
            return False
        if not 1 <= target_type <= len(_DART_TARGET_NAMES):
            return False
        self.dart_hits[side] += 1
        if target_type >= 4:
            duration = _DART_HIT_DURATIONS[0]
        else:
            duration = _DART_HIT_DURATIONS[min(self.dart_hits[side] - 1, 3)]
        self._apply_blind(side, target_type, duration)
        return True

    def _apply_blind(self, side: int, target_type: int, duration: float) -> None:
        me = self.match_elapsed()
        if me is None:
            return
        if self.blind_until[side] <= me:
            self.blind_started[side] = me
        self.blind_until[side] = max(me, self.blind_until[side]) + duration
        self.blind_type[side] = target_type

    def clear_blind(self) -> None:
        """解除手动致盲；RM_BLIND_AUTO=1 时时刻表到点仍会触发。"""
        self.blind_started = {_FACTION_RED: -1.0, _FACTION_BLUE: -1.0}
        self.blind_until = {_FACTION_RED: -1.0, _FACTION_BLUE: -1.0}
        self._blind_window = None
        self.blind_side = 0
        self.blind_target_type = 0
        self.blind_remaining_ms = 0
        self.blind_cause = 0

    def _base_defended(self, side: int) -> bool:
        """规则 5.6.1：己方完全控制己方前哨阵地时，基地获得 100% 防御增益。"""
        if side == _FACTION_RED:
            return self.zones[0] == _ZONE_RED_FULL
        return self.zones[1] == _ZONE_BLUE_FULL

    def _damage_structure(self, side: int, amount: int,
                          bypass_defense: bool = False) -> int:
        """扣基地血量并返回实际伤害，避免过量伤害污染总伤害统计。

        飞镖等特殊扣血不受 100% 防御增益影响，由调用方传 bypass_defense。
        """
        if not bypass_defense and self._base_defended(side):
            return 0
        if side == _FACTION_RED:
            applied = min(self.red_base_hp, amount)
            self.red_base_hp -= applied
        else:
            applied = min(self.blue_base_hp, amount)
            self.blue_base_hp -= applied
        self._deal_damage(side == _FACTION_RED, applied)
        if self.red_base_hp == 0 or self.blue_base_hp == 0:
            self._end_round_by_base_hp()
        return applied

    def dart_open(self, side: int) -> str | None:
        """在 30s/240s 开闸窗口内开闸。成功返回 None。"""
        if side not in self.dart:
            return "阵营必须是 1(红) 或 2(蓝)"
        me = self.match_elapsed()
        if me is None:
            return "比赛未开始"
        state = self.dart[side]
        if state["used"] >= _DART_MAX_PER_ROUND:
            return "本局飞镖已用完"
        for unlock in _DART_UNLOCK_SEC:
            if unlock <= me < unlock + _DART_FIRE_WINDOW_SEC and unlock not in state["chances"]:
                state["chances"].add(unlock)
                state["open_at"] = me
                state["close_at"] = me + _DART_GATE_OPEN_SEC + _DART_FIRE_WINDOW_SEC
                faction = "红方" if side == _FACTION_RED else "蓝方"
                self.events.append((1 if side == _FACTION_RED else 0,
                                    f"{faction}飞镖闸门开启", side))
                return None
        return "当前不在开闸窗口（30s / 240s 起各 30s）"

    def dart_shoot(self, side: int, target_type: int) -> str | None:
        """发射一枚飞镖。目标类型 1 前哨站 / 2-5 基地目标。成功返回 None。"""
        if side not in self.dart:
            return "阵营必须是 1(红) 或 2(蓝)"
        if not 1 <= target_type <= len(_DART_TARGET_NAMES):
            return f"目标类型必须是 1-{len(_DART_TARGET_NAMES)}"
        me = self.match_elapsed()
        if me is None:
            return "比赛未开始"
        state = self.dart[side]
        if state["open_at"] < 0 or not (
                state["open_at"] + _DART_GATE_OPEN_SEC <= me <= state["close_at"]):
            return "闸门未开或发射窗口已关闭"
        if state["used"] >= _DART_MAX_PER_ROUND:
            return "本局飞镖已用完"
        if me < state["cooldown_until"]:
            return f"飞镖冷却中，剩余 {state['cooldown_until'] - me:.1f}s"
        last = state["last_hit"].get(target_type, -1e9)
        if me - last < _DART_TARGET_COOLDOWN_SEC:
            return "同一目标 2 秒内不能重复命中"
        state["used"] += 1
        state["cooldown_until"] = me + _DART_COOLDOWN_SEC
        state["last_hit"][target_type] = me
        victim = _FACTION_BLUE if side == _FACTION_RED else _FACTION_RED
        damage = _DART_DAMAGE[target_type - 1]
        dealt = damage
        if target_type == 1:
            if victim == _FACTION_BLUE:
                applied = min(self.blue_outpost_hp, damage)
                self.blue_outpost_hp -= applied
                if self.blue_outpost_hp == 0:
                    self._destroy_outpost(False, side)
            else:
                applied = min(self.red_outpost_hp, damage)
                self.red_outpost_hp -= applied
                if self.red_outpost_hp == 0:
                    self._destroy_outpost(True, side)
            self._deal_damage(victim == _FACTION_RED, applied)
            dealt = applied
        else:
            dealt = self._damage_structure(victim, damage, bypass_defense=True)
            fraction = _DART_ROBOT_HP_FRACTION[target_type - 1]
            if fraction:
                if (victim == _SELF_FACTION and self.hp > 0
                        and not self.is_invincible(SELF_ROBOT_ID)):
                    applied = min(self.hp, int(self.max_hp * fraction))
                    self.hp -= applied
                    self._deal_damage(True, applied)
                    if self.hp == 0:
                        self._sync_respawn(SELF_ROBOT_ID)
                        self._register_kill(True)
                        self.events.append(
                            (3, f"{_robot_name(SELF_ROBOT_ID)}阵亡", _SELF_FACTION))
                for rid, hp in self.other_hp.items():
                    # 规则 5.7.3：移动目标只对存活的地面机器人扣血，空中机器人除外。
                    if (hp > 0 and rid % 100 != 6 and not self.is_invincible(rid)
                            and (rid in _RED_IDS) == (victim == _FACTION_RED)):
                        applied = min(hp, int(self.other_max_hp[rid] * fraction))
                        self.other_hp[rid] = hp - applied
                        self._deal_damage(victim == _FACTION_RED, applied)
                        if self.other_hp[rid] == 0:
                            self._sync_respawn(rid)
                            self._register_kill(victim == _FACTION_RED)
        if target_type >= 4:
            duration = _DART_HIT_DURATIONS[0]
        else:
            duration = _DART_HIT_DURATIONS[min(state["fixed_hits"], 3)]
            state["fixed_hits"] += 1
        self.dart_hits[side] += 1
        self._apply_blind(victim, target_type, duration)
        faction = "红方" if side == _FACTION_RED else "蓝方"
        self.events.append((1 if side == _FACTION_RED else 0,
                            f"{faction}飞镖命中{_DART_TARGET_NAMES[target_type - 1]}，"
                            f"{dealt} 伤害", side))
        return None

    def buy_ammo(self, caliber: int, rounds: int) -> str | None:
        """非远程兑换允许发弹量；模拟器假定机器人此时位于增益点。"""
        if caliber not in _PELLET_DAMAGE:
            return "口径必须是 17 或 42"
        if self.match_elapsed() is None:
            return "比赛未开始或已结束"
        if self.hp == 0:
            return "机器人已阵亡"
        if not self.launcher_ready:
            return "未安装发射机构"
        if caliber != self.caliber:
            return f"该机器人使用 {self.caliber}mm 发射机构"
        cost = rules.ammo_exchange_cost(caliber, rounds, remote=False)
        if cost is None:
            unit = rules.AMMO_EXCHANGE[(caliber, False)][0]
            return f"发数必须是 {unit} 的整数倍"
        if self.team_ammo_bought[caliber] + rounds > rules.AMMO_CAP[caliber]:
            return f"超出全队 {caliber}mm 兑换上限"
        if self.coin < cost:
            return f"金币不足，需要 {cost}"
        self._spend_coins(cost)
        self.team_ammo_bought[caliber] += rounds
        self.ammo += rounds
        return None

    def shoot(self, caliber: int, count: int = 1) -> str | None:
        """逐发结算热量与伤害；热量锁定后剩余弹丸不发射。"""
        if caliber not in _PELLET_DAMAGE:
            return "口径必须是 17 或 42"
        if not 1 <= count <= 99:
            return "弹丸数量必须是 1-99"
        if self.match_elapsed() is None:
            return "比赛未开始或已结束"
        if self.hp == 0:
            return "机器人已阵亡"
        if self.is_weak(SELF_ROBOT_ID):
            return "复活虚弱中，发射机构锁定"
        if not self.launcher_ready:
            return "未安装发射机构"
        if caliber != self.caliber:
            return f"该机器人使用 {self.caliber}mm 发射机构"
        if self.heat_permanent_lock:
            return "发射机构已永久锁定"
        target = self.target_id
        if not self.target_locked or target == 0 or target not in self.other_hp:
            return "没有锁定的敌方目标"
        if self.is_invincible(target):
            return "锁定目标处于无敌状态"
        hp = self.other_hp[target]
        if hp == 0:
            return "锁定目标已阵亡"
        if self.ammo <= 0:
            return "弹量不足，剩余 0"

        shots = min(count, self.ammo)
        fired = 0
        damage = 0
        while fired < shots and not self.heat_locked:
            self.heat += _PELLET_HEAT[caliber]
            fired += 1
            damage += _PELLET_DAMAGE[caliber]
            if self.heat >= rules.q2_limit(caliber, self.heat_limit):
                self.heat_permanent_lock = True
                break
            if self.heat > self.heat_limit:
                self.heat_locked = True
        if fired == 0:
            return "射击热量超限"

        self.ammo -= fired
        total = min(hp, damage)
        self.other_hp[target] = hp - total
        self._sync_respawn(target)
        victim_is_red = target in _RED_IDS
        if victim_is_red:
            self.blue_total_damage += total
            self.blue_hits += fired
        else:
            self.red_total_damage += total
            self.red_hits += fired
        if self.other_hp[target] == 0:
            self._register_kill(victim_is_red)
            self.events.append((1 if victim_is_red else 0,
                                f"{_robot_name(target)}阵亡", 1 if victim_is_red else 2))
        return None

    def set_module(self) -> str | None:
        """重装安装模块化 42mm 发射机构：600 HP、Q0=300、冷却 100/s。"""
        if _SELF_LOCAL_ID != 1:
            return "只有重装机器人可安装模块化 42mm 发射机构"
        if self.launcher_ready:
            return "模块化 42mm 发射机构已安装"
        gained = rules.HEAVY_MAX_HP - _ROBOT_MAX_HP[1]
        self.launcher_ready = True
        self.caliber = 42
        self.heat_limit = rules.HEAVY_HEAT_LIMIT
        self.cooling_rate = rules.HEAVY_COOLING
        self.max_hp = rules.HEAVY_MAX_HP
        if self.hp > 0:
            self.hp = min(self.max_hp, self.hp + gained)
        self.events.append((0, "重装机器人安装模块化 42mm 发射机构", _SELF_FACTION))
        return None

    def stage(self) -> tuple[int, int]:
        """赛外→准备→自检→倒计时→比赛→结算，按 elapsed 自动推进。"""
        with self._round_lock:
            e = self.elapsed
            for _ in range(8):
                sid = self.stage_id
                if sid == _STAGE_IDLE:
                    return _STAGE_IDLE, 0
                if sid == _STAGE_SETTLEMENT:
                    if self.match_over:
                        return _STAGE_SETTLEMENT, 0
                    end = self.stage_start_e + _SETTLEMENT_SEC
                    if e < end:
                        return _STAGE_SETTLEMENT, max(0, math.ceil(end - e))
                    self.start_next_round()
                    self._enter_stage(_STAGE_PREP, end)
                    continue
                end = self.stage_start_e + self._stage_duration(sid)
                if e < end:
                    return sid, max(0, math.ceil(end - e))
                if sid == _STAGE_MATCH:
                    if self.round_end_reason == 0:
                        self._finish_round_by_hp()
                    self._enter_stage(_STAGE_SETTLEMENT,
                                      self.round_start_e + self.round_len)
                elif sid == _STAGE_PREP:
                    self._enter_stage(_STAGE_CHECK, end)
                elif sid == _STAGE_CHECK:
                    self._enter_stage(_STAGE_COUNTDOWN, end)
                else:
                    self._enter_match(end)
            return self.stage_id, 0

    def _finish_round_by_hp(self) -> None:
        """时间耗尽时按规则 5.8 的顺序判定：控制度、总伤害、总剩余血量、平局。"""
        red_remain = self.hp + sum(
            hp for rid, hp in self.other_hp.items() if rid in _RED_IDS)
        blue_remain = sum(
            hp for rid, hp in self.other_hp.items() if rid not in _RED_IDS)
        for red, blue, label in (
                (self.red_control_points, self.blue_control_points, "累计控制度"),
                (self.red_total_damage, self.blue_total_damage, "全队攻击伤害"),
                (red_remain, blue_remain, "全队总剩余血量")):
            if red != blue:
                winner = 1 if red > blue else 2
                side = "红方" if winner == 1 else "蓝方"
                self._end_round(winner, 2, f"时间到，{side}{label}占优，本局获胜")
                return
        self.round_winner = 0
        self.round_end_reason = 2
        self.events.append((0, "时间到，双方控制度、伤害与剩余血量相同，本局平局", 0))

    def _end_round_by_base_hp(self) -> None:
        # 规则 5.8.1：基地被击毁时比较双方基地剩余血量,高者获胜;相同则平局。
        if self.red_base_hp == self.blue_base_hp:
            self._end_round(0, 1, "双方基地同时被击毁且剩余血量相同，本局平局")
        elif self.red_base_hp > self.blue_base_hp:
            self._end_round(1, 1, "基地被击毁，红方剩余基地血量更高，本局获胜")
        else:
            self._end_round(2, 1, "基地被击毁，蓝方剩余基地血量更高，本局获胜")

    def _end_round(self, winner: int, reason: int, text: str) -> None:
        with self._round_lock:
            if self.round_end_reason:
                return
            self.round_winner = winner
            self.round_end_reason = reason
            self.round_len = max(0.0, self.elapsed - self.round_start_e)
            if winner == 1:
                self.red_score += 1
            elif winner == 2:
                self.blue_score += 1
            self.events.append((1 if winner == 1 else 0, text, winner))
            if self.red_score >= 2 or self.blue_score >= 2:
                self.match_over = True

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
            self._locked_winner = self.round_winner if self.round_winner is not None else 0
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

    def tick_10hz(self) -> None:
        if self.stage()[0] != 4:
            # 非比赛阶段也要对齐时基,否则开赛瞬间会一次性补发整段准备时间的控制度。
            self._zone_last = self.elapsed
            self._heat_last = self.elapsed
            self._respawn_last = self.elapsed
            return
        heat_now = self.elapsed
        heat_delta = min(max(heat_now - self._heat_last, 0.0), 1.0)
        self._heat_last = heat_now
        if self.heat > 0:
            self.heat = max(0.0, self.heat - self.cooling_rate * heat_delta)
            if self.heat == 0:
                self.heat_locked = False
        me = max(0.0, self.elapsed - self.round_start_e)
        while self._econ_paid < len(_ECONOMY_MILESTONES):
            at, amount = _ECONOMY_MILESTONES[self._econ_paid]
            if me < at:
                break
            self.red_economy += amount
            self.blue_economy += amount
            self._econ_paid += 1
        # 世界推进必须放在自机阵亡的提前返回之前：基地、前哨站、堡垒和控制度
        # 都与自机是否存活无关，放在后面会让自机一死整场比赛悄悄冻结。
        self.tick_field_hp()
        self.tick_fortress()
        self.tick_zones()
        self.tick_respawn()
        if self.hp == 0:
            return

        if not self.auto_hp:
            return
        if not self.is_invincible(SELF_ROBOT_ID) and self.rng.random() < 0.05:
            dmg = self.rng.randint(5, 40)
            applied = min(self.hp, dmg)
            self.hp -= applied
            self._deal_damage(True, applied)
            if self.hp == 0:
                self._sync_respawn(SELF_ROBOT_ID)
                self.events.append((3, f"{_robot_name(SELF_ROBOT_ID)}阵亡", _SELF_FACTION))
        if self.hp > 0 and self.rng.random() < 0.03:
            self.hp = min(self.max_hp, self.hp + 60)

    def _expire_temp_control(self, index: int) -> None:
        """临时控制的 5s 激活进度归零：由另一方转为完全控制（表 5-21）。"""
        if index >= 2:
            self.zones[index] = _ZONE_RED_FULL if index == 2 else _ZONE_BLUE_FULL
            return
        holder = _FACTION_RED if self.zones[index] == _ZONE_RED_TEMP else _FACTION_BLUE
        challenger = _FACTION_BLUE if holder == _FACTION_RED else _FACTION_RED
        self.zones[index] = _ZONE_BLUE_FULL if challenger == _FACTION_BLUE else _ZONE_RED_FULL
        if index == 0 and self.red_outpost_hp == 0:
            self.red_outpost_hp = _OUTPOST_REBUILD_HP
        elif index == 1 and self.blue_outpost_hp == 0:
            self.blue_outpost_hp = _OUTPOST_REBUILD_HP
        side_name = "蓝方" if challenger == _FACTION_BLUE else "红方"
        zone_name = "红方" if index == 0 else "蓝方"
        self.events.append((2 if challenger == _FACTION_RED else 0,
                            f"{side_name}完全控制{zone_name}前哨区", challenger))

    def tick_zones(self) -> None:
        """按比赛秒结算控制度与加固机会。"""
        now = self.elapsed
        delta = min(max(now - self._zone_last, 0.0), 1.0)
        self._zone_last = now
        for index, state in enumerate(self.zones):
            if state not in (_ZONE_RED_TEMP, _ZONE_BLUE_TEMP):
                continue
            self.zone_progress[index] = max(0.0, self.zone_progress[index] - delta)
            if self.zone_progress[index] == 0:
                self._expire_temp_control(index)

        self._zone_accum += delta
        steps = int(self._zone_accum)
        if steps:
            self._zone_accum -= steps
            red_outpost = all(state in (_ZONE_RED_TEMP, _ZONE_RED_FULL)
                              for state in self.zones[:2])
            blue_outpost = all(state in (_ZONE_BLUE_TEMP, _ZONE_BLUE_FULL)
                               for state in self.zones[:2])
            red_base = all(state in (_ZONE_RED_TEMP, _ZONE_RED_FULL)
                           for state in self.zones[2:])
            blue_base = all(state in (_ZONE_BLUE_TEMP, _ZONE_BLUE_FULL)
                            for state in self.zones[2:])
            if red_outpost:
                self.red_control_points += _CONTROL_OUTPOST_RATE * steps
            if blue_outpost:
                self.blue_control_points += _CONTROL_OUTPOST_RATE * steps
            if red_base:
                self.red_control_points += _CONTROL_BASE_RATE * steps
            if blue_base:
                self.blue_control_points += _CONTROL_BASE_RATE * steps

        self._reinforce_accum += delta
        while self._reinforce_accum >= _REINFORCE_PERIOD_SEC:
            self._reinforce_accum -= _REINFORCE_PERIOD_SEC
            self.red_reinforce_chances += 1
            self.blue_reinforce_chances += 1

    def tick_fortress(self) -> None:
        if self.fortress_auto and self.rng.random() < 0.01:
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
            self.blue_hits += 1
        else:
            self.red_total_damage += amount
            self.red_hits += 1

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
        if not self.auto_hp:
            return
        for robot_id, hp in self.other_hp.items():
            if hp == 0:
                continue
            if not self.is_invincible(robot_id) and self.rng.random() < 0.05:
                applied = min(hp, self.rng.randint(5, 40))
                self.other_hp[robot_id] = hp - applied
                self._deal_damage(robot_id in _RED_IDS, applied)
                if self.other_hp[robot_id] == 0:
                    self._sync_respawn(robot_id)
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
                self._destroy_outpost(False, _FACTION_RED)
        if self.rng.random() < 0.018 and self.red_outpost_hp > 0:
            applied = min(self.red_outpost_hp, self.rng.randint(20, 80))
            self.red_outpost_hp -= applied
            self._deal_damage(True, applied)
            if self.red_outpost_hp == 0:
                self._destroy_outpost(True, _FACTION_BLUE)

        if self.rng.random() < 0.02:
            applied = (0 if self._base_defended(_FACTION_BLUE)
                       else min(self.blue_base_hp, self.rng.randint(20, 90)))
            if applied:
                self.blue_base_hp -= applied
                self._deal_damage(False, applied)
        if self.rng.random() < 0.018:
            applied = (0 if self._base_defended(_FACTION_RED)
                       else min(self.red_base_hp, self.rng.randint(20, 90)))
            if applied:
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

        if self.blue_base_hp == 0 or self.red_base_hp == 0:
            self._end_round_by_base_hp()

    def start_next_round(self) -> None:
        self.round_index += 1
        self.round_start_e = self.elapsed
        self.round_len = float(_MATCH_SEC)
        self.round_end_reason = 0
        self.round_winner = None
        self._locked_winner = None
        self.red_base_hp = _BASE_MAX_HP
        self.blue_base_hp = _BASE_MAX_HP
        self.red_outpost_hp = _OUTPOST_MAX_HP
        self.blue_outpost_hp = _OUTPOST_MAX_HP
        self.red_economy = _INITIAL_ECONOMY
        self.blue_economy = _INITIAL_ECONOMY
        self._econ_paid = 0
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
        self.red_hits = 0
        self.blue_hits = 0
        self.dart_hits = {_FACTION_RED: 0, _FACTION_BLUE: 0}
        self.dart = {side: _new_dart_state() for side in (_FACTION_RED, _FACTION_BLUE)}
        self.clear_blind()
        self.zones = list(_INITIAL_ZONES)
        self.zone_progress = [0.0] * len(self.zones)
        self.red_control_points = 0
        self.blue_control_points = 0
        self.red_reinforce_chances = 0
        self.blue_reinforce_chances = 0
        self._zone_accum = 0.0
        self._reinforce_accum = 0.0
        self._zone_last = self.elapsed
        for rid in self.other_hp:
            self.other_hp[rid] = self.other_max_hp[rid]
        self.hp = self.max_hp
        self.heat = 0.0
        self.heat_locked = False
        self.heat_permanent_lock = False
        self._heat_last = self.elapsed
        self.ammo = rules.SENTRY_INITIAL_AMMO if _SELF_LOCAL_ID == 7 else 0
        self.team_ammo_bought = {17: 0, 42: 0}
        self.respawn_state = {
            robot_id: _new_respawn_state() for robot_id in self.respawn_state
        }
        self._respawn_last = self.elapsed

    @property
    def coin(self) -> int:
        """金币归队伍共享，自机面板显示的是己方队伍余额。"""
        return self.red_economy if _SELF_FACTION == _FACTION_RED else self.blue_economy

    def _spend_coins(self, cost: int) -> None:
        if _SELF_FACTION == _FACTION_RED:
            self.red_economy -= cost
        else:
            self.blue_economy -= cost

    def field_health(self) -> list[tuple[int, int, int, int]]:
        out: list[tuple[int, int, int, int]] = [
            (SELF_ROBOT_ID, _SELF_FACTION, self.hp, self.max_hp)
        ]
        for robot_id, hp in self.other_hp.items():
            faction = _SELF_FACTION if robot_id in _FRIENDLY_IDS else (
                _FACTION_BLUE if _SELF_FACTION == _FACTION_RED else _FACTION_RED)
            out.append((robot_id, faction, hp, self.other_max_hp[robot_id]))
        return out

    def match_elapsed(self) -> float | None:
        stage, _ = self.stage()
        if stage != 4:
            return None
        return max(0.0, self.elapsed - self.round_start_e)

    def tick_blind(self) -> None:
        """推进致盲状态（本地模拟，非 2027 协议）。双方独立计时，取剩余更久的一方上报。"""
        me = self.match_elapsed()
        active: tuple[float, float, int, int] | None = None
        if me is not None:
            side = 0
            for candidate in (_FACTION_RED, _FACTION_BLUE):
                if self.blind_until[candidate] > me and (
                        side == 0 or self.blind_until[candidate] > self.blind_until[side]):
                    side = candidate
            if side:
                start = self.blind_started[side]
                duration = max(0.0, self.blind_until[side] - start)
                active = (start, duration, side, self.blind_type[side])
            if active is None and _BLIND_AUTO:
                for start, duration in _BLIND_SCHEDULE:
                    if start <= me < start + duration:
                        active = (start, duration, _FACTION_RED, 1)
                        break

        if active != self._blind_window:
            if active is not None:
                start, duration, side, target_type = active
                self._blind_window = active
                self.blind_side = side
                self.blind_target_type = target_type
                self.blind_cause = _BLIND_CAUSE_DART
                # 编造字段：用比赛内 ms 而非挂钟 ms，保证两次运行完全一致。
                self.blind_started_ms = int(start * 1000)
                faction = "红方" if side == _FACTION_RED else "蓝方"
                target = _DART_TARGET_NAMES[target_type - 1]
                self.events.append(
                    (3, f"{faction}基地被飞镖命中（{target}），图传致盲", side))
                print(f"[server] 致盲开始 {faction} 目标={target} "
                      f"match_t={start:.1f}s 持续={duration:.1f}s", flush=True)
            else:
                self._blind_window = None
                self.blind_side = 0
                self.blind_target_type = 0
                self.blind_cause = 0
                self.blind_remaining_ms = 0
                self.events.append((0, "基地致盲结束，图传恢复", 0))
                print("[server] 致盲结束", flush=True)

        if self._blind_window is not None and me is not None:
            start, duration, _side, _target = self._blind_window
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
            faction = _SELF_FACTION if robot_id in _FRIENDLY_IDS else (
                _FACTION_BLUE if _SELF_FACTION == _FACTION_RED else _FACTION_RED)
            x = _FIELD_LENGTH_M / 2 + (_FIELD_LENGTH_M / 2 - 2.0) * math.sin(e * speed + phase)
            y = _FIELD_WIDTH_M / 2 + (_FIELD_WIDTH_M / 2 - 1.5) * math.cos(e * speed * 0.8 + phase)
            poses.append((robot_id, faction, x, y, math.degrees(math.sin(e * speed + phase))))
        return poses


def _publish(client: mqtt.Client, topic: str, msg) -> None:
    client.publish(topic, msg.SerializeToString(), qos=0)


def _on_connect(client, userdata, flags, reason_code, properties=None):
    print(f"[server] MQTT 已连接 ({reason_code})，订阅终端下行指令")
    client.subscribe([(T_CUSTOM_CONTROL, 0), (T_COMMON_COMMAND, 0), (T_SIM_LINK_CAR, 0)])


def _on_message(client, userdata, message):
    if message.topic == T_CUSTOM_CONTROL:
        # 终端把机械臂 0x0302 帧的 30 字节数据段原样转来：前 24 字节是 6 个关节角。
        m = pb.CustomControl()
        try:
            m.ParseFromString(message.payload)
        except Exception:
            return
        if len(m.data) >= 24:
            userdata.arm_joints = decode_joints_deg(m.data)
            userdata.arm_at = time.monotonic()
            userdata.arm_frames += 1
    elif message.topic == T_SIM_LINK_CAR:
        online = message.payload != b"off"
        if online != userdata.car_online:
            print(f"[server] 车端数据 {'恢复' if online else '断电（RobotTelemetry 停发）'}")
        userdata.car_online = online
    elif message.topic == T_COMMON_COMMAND:
        m = pb.CommonCommand()
        m.ParseFromString(message.payload)
        name = _CTRL_NAMES.get(m.command_id)
        if name:
            print(f"[server] 收到赛程控制 <- {name} (id=0x{m.command_id:02X})")
            userdata.apply_control(m.command_id, m.param)
        else:
            print(f"[server] 收到 CommonCommand <- 终端：id={m.command_id} param={m.param}")


def main() -> int:
    match = SimulatedMatch()
    match.car_online = True
    match.arm_joints = None
    match.arm_at = None
    match.arm_frames = 0
    match.arm_moving = False

    def apply_control(cmd: int, param: int = 0) -> None:
        if cmd == CTRL_BEGIN:
            match.begin()
            print(f"[server] 继续赛程,当前阶段 {match.stage()[0]}")
        elif cmd == CTRL_HOLD:
            match.hold()
            print(f"[server] 已暂停在赛程 {match.hold_at:.1f}s")
        elif cmd == CTRL_RESET:
            match.reset()
            print("[server] 已复位到赛外,等待推进")
        elif cmd == CTRL_PREP:
            match.prep()
            print(f"[server] 进入准备阶段 {_PREP_SEC}s")
        elif cmd == CTRL_CHECK:
            match.check()
            print(f"[server] 进入裁判系统自检 {_CHECK_SEC}s")
        elif cmd == CTRL_COUNTDOWN:
            match.countdown()
            print(f"[server] 进入倒计时 {_COUNTDOWN_SEC}s")
        elif cmd == CTRL_RESUME:
            match.begin()
            print(f"[server] 已继续赛程,阶段 {match.stage()[0]}")
        elif cmd == CTRL_MODULE:
            error = match.set_module()
            if error is None:
                print(f"[server] 重装已安装 42mm 模块,HP={match.max_hp},"
                      f"Q0={match.heat_limit}")
            else:
                print(f"[server] 安装失败: {error}", file=sys.stderr)
        elif cmd == CTRL_BUY:
            caliber, rounds = divmod(param, 10000)
            error = match.buy_ammo(caliber, rounds)
            if error is None:
                print(f"[server] 兑换 {caliber}mm x{rounds},队伍余额 {match.coin}")
            else:
                print(f"[server] 兑换失败: {error}", file=sys.stderr)
        elif cmd == CTRL_PAY_REVIVE:
            error = match.pay_revive()
            if error is None:
                print(f"[server] 已兑换立即复活,HP={match.hp},队伍余额 {match.coin}")
            else:
                print(f"[server] 兑换复活失败: {error}", file=sys.stderr)
        elif cmd == CTRL_CARD:
            error = match.card_scan(param)
            if error is None:
                print(f"[server] 场地交互卡已解除虚弱,HP={match.hp}")
            else:
                print(f"[server] 交互卡无效: {error}", file=sys.stderr)
        elif cmd == CTRL_SEEK:
            # 面板/命令行给的是局内比赛秒，内部时钟仍是全局赛程。
            target = (match.round_start_e + float(param)
                      if match.stage()[0] in (_STAGE_MATCH, _STAGE_SETTLEMENT)
                      else float(param))
            match.seek(target)
            print(f"[server] 赛程跳到 {match.elapsed:.1f}s,阶段 {match.stage()[0]}")
        elif cmd == CTRL_SPEED:
            match.set_speed(max(1, param) / 10.0)
            print(f"[server] 倍速设为 {match.speed:.1f}x")
        elif cmd == CTRL_SET_HP:
            robot_id, hp = divmod(param, 1000)
            if match.set_hp(robot_id, hp):
                print(f"[server] {_robot_name(robot_id)} 血量设为 {hp}")
            else:
                print(f"[server] 未知机器人 ID {robot_id},血量未改", file=sys.stderr)
        elif cmd == CTRL_AUTO_HP:
            match.auto_hp = bool(param)
            print(f"[server] 随机掉血{'开启' if match.auto_hp else '关闭'}")
        elif cmd == CTRL_SET_ZONE:
            zone, state = divmod(param, 10)
            if match.set_zone(zone, state):
                print(f"[server] {_ZONE_NAMES[zone]} -> {_ZONE_STATE_NAMES[state]}")
            else:
                print(f"[server] 非法阵地设置 序号={zone} 状态={state}", file=sys.stderr)
        elif cmd == CTRL_SET_STRUCTURE_HP:
            index, hp = divmod(param, 10000)
            if match.set_structure_hp(index, hp):
                print(f"[server] {_STRUCTURE_NAMES[index]} 血量设为 "
                      f"{min(hp, _STRUCTURE_MAX_HP[index])}")
            else:
                print(f"[server] 非法建筑序号 {index}", file=sys.stderr)
        elif cmd == CTRL_SET_FORTRESS:
            holders = {0: "未占领", 1: "红方", 2: "蓝方", 3: "随机"}
            if match.set_fortress(param):
                print(f"[server] 堡垒 -> {holders[param]}")
            else:
                print(f"[server] 非法堡垒参数 {param}", file=sys.stderr)
        elif cmd == CTRL_USE_REINFORCE:
            faction = "红方" if param == _FACTION_RED else "蓝方"
            error = match.use_reinforce(param)
            if error is None:
                print(f"[server] {faction}消耗 1 次加固机会")
            else:
                print(f"[server] {faction}加固失败: {error}", file=sys.stderr)
        elif cmd == CTRL_BLIND:
            side, target_type = divmod(param, 10)
            if match.trigger_blind(side, target_type):
                faction = "红方" if side == _FACTION_RED else "蓝方"
                print(f"[server] 手动致盲 {faction} "
                      f"目标={_DART_TARGET_NAMES[target_type - 1]}")
            else:
                print(f"[server] 非法致盲参数 阵营={side} 类型={target_type}",
                      file=sys.stderr)
        elif cmd == CTRL_BLIND_CLEAR:
            match.clear_blind()
            print("[server] 已解除手动致盲")
        elif cmd == CTRL_SHOOT:
            caliber, count = divmod(param, 100)
            error = match.shoot(caliber, count)
            if error is None:
                print(f"[server] {caliber}mm x{count} 命中 {_robot_name(match.target_id)}")
            else:
                print(f"[server] 射击失败: {error}", file=sys.stderr)
        elif cmd == CTRL_DART_OPEN:
            error = match.dart_open(param)
            if error is None:
                print(f"[server] {'红方' if param == _FACTION_RED else '蓝方'}飞镖闸门开启")
            else:
                print(f"[server] 开闸失败: {error}", file=sys.stderr)
        elif cmd == CTRL_DART_SHOOT:
            side, target_type = divmod(param, 10)
            error = match.dart_shoot(side, target_type)
            if error is None:
                print(f"[server] {'红方' if side == _FACTION_RED else '蓝方'}飞镖已发射")
            else:
                print(f"[server] 发射失败: {error}", file=sys.stderr)

    match.apply_control = apply_control

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
            fire_permit=(match.hp > 0 and match.target_locked and match.launcher_ready
                         and not match.heat_locked and not match.heat_permanent_lock
                         and match.ammo > 0),
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
        if match.car_online:
            _publish(client, T_TELEMETRY, m)

    def send_arm_echo() -> None:
        # 模拟的车：超过 _ARM_STOP_AFTER_S 没收到新姿态就停住，绝不按最后一帧继续动。
        age = None if match.arm_at is None else time.monotonic() - match.arm_at
        moving = age is not None and age < _ARM_STOP_AFTER_S
        if moving != match.arm_moving:
            print(f"[server] 车上机械臂 {'开始跟随' if moving else '停住（没收到新姿态）'}")
            match.arm_moving = moving
        client.publish(T_SIM_ARM_ECHO, json.dumps({
            "moving": moving,
            "age_ms": None if age is None else int(age * 1000),
            "frames": match.arm_frames,
            "joints_deg": None if match.arm_joints is None
            else [round(value, 1) for value in match.arm_joints],
        }))

    def send_game_status() -> None:
        stage, remain = match.stage()
        _publish(client, T_GAME_STATUS, pb.GameStatus(
            current_round=match.round_index + 1, total_rounds=3,
            red_score=match.red_score, blue_score=match.blue_score,
            current_stage=stage, stage_countdown_sec=remain,
            stage_elapsed_sec=int(match.match_elapsed() or 0), is_paused=False,
            winner=match.winner(), end_reason=match.round_end_reason,
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
            red_outpost_zone=match.zones[0], blue_outpost_zone=match.zones[1],
            red_base_zone=match.zones[2], blue_base_zone=match.zones[3],
            red_control_points=match.red_control_points,
            blue_control_points=match.blue_control_points,
            red_reinforce_chances=match.red_reinforce_chances,
            blue_reinforce_chances=match.blue_reinforce_chances,
        ))
        client.publish(T_SIM_AUTO_HP, b"1" if match.auto_hp else b"0", retain=True)

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
            shooter_heat_17mm=int(match.heat), shooter_heat_limit=match.heat_limit,
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
            self_base_blinded=(match.blind_side == _SELF_FACTION),
            blind_remaining_ms=match.blind_remaining_ms,
            cause=match.blind_cause,
            affected_faction=match.blind_side,
            target_type=match.blind_target_type,
        )
        if match.blind_side:
            m.blind_started_ms = match.blind_started_ms
        _publish(client, T_BLIND_STATUS, m)

    def send_respawn() -> None:
        pending, total, current, free, cost, can_pay = match.respawn_status()
        _publish(client, T_ROBOT_RESPAWN, pb.RobotRespawnStatus(
            is_pending_respawn=pending,
            total_respawn_progress=total,
            current_respawn_progress=current,
            can_free_respawn=free,
            gold_cost_for_respawn=cost,
            can_pay_for_respawn=can_pay,
        ))

    def send_position_set() -> None:
        m = pb.RobotPositionSet()
        # 自身坐标必须复用既有 RobotPosition 轨迹，不另行编造：终端侧
        # (todo 6) 会丢弃集合内的自身坐标、只取单机路径的值，两处若不同源
        # 会让地图与数字读数对不上。
        sx, sy, syaw = _self_pose(_pose_tick(match.elapsed))
        own = m.entries.add()
        own.robot_id = SELF_ROBOT_ID
        own.faction = _SELF_FACTION
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
        threading.Thread(target=loop, args=(1.0, send_respawn), daemon=True),
        threading.Thread(target=loop, args=(1 / 20, send_events), daemon=True),
        threading.Thread(target=loop, args=(1 / 10, send_arm_echo), daemon=True),
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
