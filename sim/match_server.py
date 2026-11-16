"""RoboMaster 2027 V1.0.0 自定义终端模拟赛事引擎。"""

from __future__ import annotations

import json
import math
import os
import random
import sys
import threading
import time
from collections import deque
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
    T_ASSEMBLY_COMMAND,
    T_COMMON_COMMAND,
    T_CUSTOM_CONTROL,
    T_BUFF,
    T_INJURY,
    T_DART_COMMAND,
    T_DART_STATUS,
    T_EVENT,
    T_GAME_STATUS,
    T_GLOBAL_LOGISTICS,
    T_GLOBAL_SPECIAL,
    T_GLOBAL_UNIT,
    T_MAP_COMMAND,
    T_MAP_INFO,
    T_PATH_PLAN,
    T_KEYBOARD_MOUSE,
    T_PERFORMANCE_COMMAND,
    T_PERFORMANCE_SYNC,
    T_RADAR_INFO,
    T_ROBOT_STATIC,
    T_SIM_AUTO_HP,
    T_ROBOT_DYNAMIC,
    T_ROBOT_MODULE,
    T_ROBOT_POSITION,
    T_ROBOT_RESPAWN,
    T_RUNE_COMMAND,
    T_RUNE_STATUS,
    T_SIM_ARM_ECHO,
    T_SIM_ARM_FOLLOW,
    T_CUSTOM_BYTE_BLOCK,
    T_SIM_LINK_CAR,
    T_SENTRY_COMMAND,
    T_SENTRY_RESULT,
    T_SENTRY_STATUS,
    T_SIM_STATE,
    T_TECH_CORE,
)
from sim.arm_protocol import FOLLOW_FLAG, decode_joints_deg, feedback_payload

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
CTRL_DART_OPEN = 0x1E        # param = 阵营(1红 2蓝), 30s/240s 起各有 1 次开闸机会
CTRL_DART_SHOOT = 0x1F       # param = 阵营(1红 2蓝) x10 + 飞镖目标类型(1-5)
CTRL_PREP = 0x20             # 进入三分钟准备阶段
CTRL_CHECK = 0x21            # 进入十五秒裁判系统自检
CTRL_COUNTDOWN = 0x22        # 进入五秒倒计时
CTRL_RESUME = 0x23           # 从暂停处继续,不跳阶段
CTRL_MODULE = 0x24           # 重装安装模块化 42mm 发射机构(重装专用)
CTRL_BUY = 0x25              # param = 口径 x10000 + 发数,非远程兑换允许发弹量
CTRL_PAY_REVIVE = 0x26       # 金币兑换立即复活(无参数)
CTRL_CARD = 0x27             # param = 机器人 ID(0=自机),模拟检测到场地交互卡
CTRL_MARK = 0x28             # param = 标记类型(1攻击 2防御 3警戒),模拟指挥手发一个地图标记
CTRL_EXCHANGE_POINT = 0x29   # param = 1 车开进兑换点(补给区/基地增益点/前哨站增益点), 0 开出去
CTRL_ASSEMBLY_ZONE = 0x2A    # param = 1 重装开进己方装配区, 0 开出去
CTRL_VERIFY_LAUNCHER = 0x2B  # 重装打出验证弹,成功安装模块化 42mm 发射机构(重装专用)
CTRL_ENEMY_RUNE = 0x2C       # param = 1 敌方激活小能量机关, 2 大能量机关(只发 Event 3/4,我方状态不变)
CTRL_TERRAIN = 0x2D          # param = 1 高地 2 公路 3 跨越隧道 4 穿越隧道 5 踩了高地下面的卡,自机拿地形增益
CTRL_RADAR_LOCK = 0x2E       # param = 敌方车号(0 解除),强制雷达把它标成「被标记」,测地图上的标记
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
    CTRL_EXCHANGE_POINT: "车开进/开出兑换点",
    CTRL_ASSEMBLY_ZONE: "重装开进/开出装配区",
    CTRL_VERIFY_LAUNCHER: "打验证弹",
    CTRL_ENEMY_RUNE: "敌方激活能量机关",
    CTRL_TERRAIN: "自机拿地形跨越增益",
    CTRL_RADAR_LOCK: "强制雷达标记",
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

# RM_BLIND_AUTO=1：敌方飞镖在比赛第 30.0s、150.0s 命中我方基地随机移动目标，各遮挡 10s
# （规则 5.7.3）。终端只收到 Event 9，遮挡时长由终端自己按规则算。
_BLIND_SCHEDULE: tuple[tuple[float, int], ...] = ((30.0, 4), (150.0, 4))
_BLIND_AUTO = os.environ.get("RM_BLIND_AUTO", "0") == "1"

# 阵容编号对齐 RMUC 2027 规则手册表 3-1 和通信协议附录二：1 重装 / 3,4 步兵 / 6 空中 /
# 7 哨兵，蓝方 +100。终端席位只有 1/3/4/6（哨兵没有操作手）；场上有血量的是 1/3/4/7。
# 编号→兵种的对应见 cpp/presentation.cpp robot_class_name()，两处必须一致。
_FACTION_RED = 1
_FACTION_BLUE = 2
_RED_LINEUP: tuple[int, ...] = (1, 3, 4, 6)
_BLUE_LINEUP: tuple[int, ...] = (101, 103, 104, 106)
_RED_FIELD_IDS: tuple[int, ...] = (1, 3, 4, 7)
_BLUE_FIELD_IDS: tuple[int, ...] = (101, 103, 104, 107)
if SELF_ROBOT_ID in _RED_LINEUP:
    _SELF_FACTION = _FACTION_RED
elif SELF_ROBOT_ID in _BLUE_LINEUP:
    _SELF_FACTION = _FACTION_BLUE
else:
    raise ValueError(f"RM_SELF_ID={SELF_ROBOT_ID} 不是 2027 席位 {_RED_LINEUP + _BLUE_LINEUP}")
_ENEMY_FACTION = _FACTION_BLUE if _SELF_FACTION == _FACTION_RED else _FACTION_RED
# 自机以外的场上机器人。指挥手（6）不在场上，己方 4 台都算「别的车」。
_FRIENDLY_IDS: tuple[int, ...] = tuple(
    i for i in (_RED_FIELD_IDS if _SELF_FACTION == _FACTION_RED else _BLUE_FIELD_IDS)
    if i != SELF_ROBOT_ID)
_ENEMY_IDS: tuple[int, ...] = _BLUE_FIELD_IDS if _SELF_FACTION == _FACTION_RED else _RED_FIELD_IDS
_SELF_LOCAL_ID = SELF_ROBOT_ID - 100 if SELF_ROBOT_ID > 100 else SELF_ROBOT_ID
# 附录二：8 红方飞镖，108 蓝方飞镖。飞镖扣血致死时记作击杀者。
_DART_ROBOT_ID = {_FACTION_RED: 8, _FACTION_BLUE: 108}


def _is_red(robot_id: int) -> bool:
    return robot_id < 100


_SIDE_NAMES = {_FACTION_RED: "红方", _FACTION_BLUE: "蓝方"}


def _opponent(side: int) -> int:
    return _FACTION_BLUE if side == _FACTION_RED else _FACTION_RED


def _zone_full(side: int) -> int:
    return _ZONE_RED_FULL if side == _FACTION_RED else _ZONE_BLUE_FULL


def _zone_temp(side: int) -> int:
    return _ZONE_RED_TEMP if side == _FACTION_RED else _ZONE_BLUE_TEMP


# 表 5-16/5-17/5-18：重装 400,步兵 200,空中 200,哨兵按自动哨兵 400。
_ROBOT_MAX_HP = {1: 400, 3: 200, 4: 200, 6: 200, 7: 400}

# 规则 5.6.1：基地 10000 HP,前哨站 1000 HP,击毁后重建 500 HP。
_BASE_MAX_HP = 10000
_OUTPOST_MAX_HP = 1000
_OUTPOST_REBUILD_HP = 500
# Event 2 的参数:按开局位置编号,R 前哨 11、B 前哨 111(通信协议附录二)。
_OUTPOST_EVENT_IDS = ("11", "111")
# 建筑控制指令的寻址顺序，与 _STRUCTURE_MAX_HP 一一对应。
_STRUCTURE_NAMES = ("红方基地", "蓝方基地", "红方前哨站", "蓝方前哨站")
_STRUCTURE_MAX_HP = (_BASE_MAX_HP, _BASE_MAX_HP, _OUTPOST_MAX_HP, _OUTPOST_MAX_HP)

# 飞镖目标类型与遮挡时长，取自规则 5.7.3：固定类目标第 1~4 次命中分别遮挡
# 10/5/3/2 秒，随机移动类每发都遮挡 10 秒。
_DART_TARGET_NAMES = ("前哨站", "基地固定目标", "基地随机固定目标",
                      "基地随机移动目标", "基地末端移动目标")
_DART_HIT_DURATIONS = (10.0, 5.0, 3.0, 2.0)
# 规则 5.7.3：每局 4 枚；30s/240s 各给 1 次开闸机会，没用的留着；开闸 7s，发射 30s；
# 闸门关上后冷却 15s 才能再开；同一目标两次命中至少间隔 2s。命中伤害与移动类目标对地面机器人的扣血比例见下表。
_DART_MAX_PER_ROUND = 4
_DART_UNLOCK_SEC = (30.0, 240.0)
_DART_GATE_OPEN_SEC = 7.0
_DART_FIRE_WINDOW_SEC = 30.0
_DART_GATE_COOLDOWN_SEC = 15.0
_DART_TARGET_COOLDOWN_SEC = 2.0
_DART_DAMAGE = (1000, 200, 300, 800, 1250)
_DART_ROBOT_HP_FRACTION = (0.0, 0.0, 0.0, 0.20, 0.50)
# 模拟的装配(规则手册 5.4.3、表 5-10):开始后科技核心先运动 3 秒到位,之后模拟操作手每 3 秒做完
# 一步。一到三级开始后 30 秒没确认算超时(模拟器自定),四级要在到位后 45 秒内装完。
_ASSEMBLY_MOVE_SEC = 3.0
_ASSEMBLY_STEP_SEC = 3.0
_ASSEMBLY_TIMEOUT_SEC = 30.0
_ASSEMBLY_LEVEL4_SEC = 45.0
# 模拟的能量机关:激活 5 秒后进入已激活,保持 45 秒。
_RUNE_ACTIVATING_SEC = 5.0
# 5.6.3：小能量机关在比赛开始、1 分 30 秒各给一次机会，大能量机关在 3 分、4 分 15 秒、5 分 30 秒
# 各给一次，机会可累积；3 分钟后进入大能量机关阶段。
_SMALL_RUNE_AT = (0.0, 90.0)
_BIG_RUNE_AT = (180.0, 255.0, 330.0)
_RUNE_ACTIVE_SEC = 45.0
# 大能量机关(表 5-22、5-23):模拟的我方总是 10 臂、平均 9.2 环 -> 攻击 300%、防御 50%、热量冷却 5 倍,60 秒;
# 模拟的敌方大能量机关 8 臂、平均 8.5 环 -> 攻击 200%、防御 25%、冷却 3 倍,45 秒。
_BIG_RUNE_EVENT = "10,9.2"
_BIG_RUNE_ACTIVE_SEC = 60.0
_BIG_RUNE_BUFFS = ((1, 300), (2, 50))
_BIG_RUNE_COOLING_TIMES = 5
_ENEMY_BIG_RUNE_EVENT = "8,8.5"
# 5.6.4.5 地形跨越增益:(名字, Buff 类型, 增益值, 秒数)。穿越隧道是 1.5 倍热量冷却,协议里类型 3 发的是
# 「每秒多冷却多少」,发送时按当前冷却速率换算。拿着一种又拿到另一种时防御变成 50%。
_TERRAIN = {
    1: ("高地", 2, 25, 30.0),
    2: ("公路", 2, 25, 5.0),
    3: ("跨越隧道", 2, 25, 30.0),
    4: ("穿越隧道", 3, 0, 120.0),
}
_TERRAIN_PRE_HIGHLAND = 5
# 5.7.1:前 3 分钟、没装上 42mm 的重装防御 50%。
_HERO_EARLY_SEC = 180.0
# 非指挥手席位:模拟指挥手每 20 秒(比赛时间)标一次敌方重装。
_MARK_PERIOD_SEC = 20.0
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

_FIELD_LENGTH_M = 28.0
_FIELD_WIDTH_M = 15.0

# 必须与 cpp/presentation.cpp robot_class_name() 逐字一致，否则事件文本与花名册对同一台车的称呼会不同。
_ROBOT_CLASS_NAMES = {1: "重装", 3: "步兵", 4: "步兵", 6: "空中", 7: "哨兵"}


def _new_dart_state() -> dict:
    return {"used": 0, "open_at": -1.0, "close_at": -1.0, "openings": 0,
            "hits": 0, "fixed_hits": 0, "last_hit": {}}


def _new_injury() -> dict:
    return {"collision": 0, "small": 0, "large": 0, "dart": 0, "module": 0, "offline": 0, "penalty": 0,
            "server": 0}


def _new_respawn_state() -> dict:
    return {"dead": False, "progress": 0.0, "total": 0, "paid": 0,
            "weak": False, "weak_until": None, "invincible_until": 0.0,
            "revived_at": 0.0, "revived_paid": False}


def _robot_name(robot_id: int) -> str:
    number = robot_id - 100 if robot_id > 100 else robot_id
    faction = "红方" if _is_red(robot_id) else "蓝方"
    return f"{faction}{_ROBOT_CLASS_NAMES.get(number, str(number))}"


def _self_pose(e: float) -> tuple[float, float, float]:
    """自身位姿的唯一函数来源，RobotPosition 与雷达消息里的自机都必须走它。

    调用方必须传入 _pose_tick() 量化后的时间：两个 sender 跑在各自的 1Hz 线程
    上，若各自直接读 match.elapsed，采样点会相差几十微秒，同一时刻的两条消息
    就会带上毫米级不同的自身坐标，与「同源」的说法不符。
    """
    return (14.0 + 5.5 * math.sin(e * 0.2),
            7.5 + 3.5 * math.cos(e * 0.17),
            math.degrees(math.sin(e * 0.3)))


def _pose_tick(elapsed: float) -> float:
    return math.floor(elapsed * 10.0) / 10.0


def relative_outpost_status(absolute: int, self_faction: int) -> int:
    """Convert an absolute zone owner to the protocol's self-relative outpost code."""
    if absolute == 0:
        return 0
    if self_faction == _FACTION_RED:
        return {2: 1, 1: 2, 3: 3, 4: 4}.get(absolute, 0)
    return {4: 1, 3: 2, 1: 3, 2: 4}.get(absolute, 0)


def relative_base_shield(absolute: int, self_faction: int, enemy: bool) -> int:
    """Convert an absolute base-zone state to the protocol's self-relative code."""
    if self_faction == _FACTION_RED:
        full, temporary = ((4, 1) if enemy else (2, 3))
    else:
        full, temporary = ((2, 3) if enemy else (4, 1))
    if absolute == full:
        return 1
    if absolute == temporary:
        return 2
    return 0


def ordered_robot_ids(self_faction: int) -> tuple[int, ...]:
    """Return the official GlobalUnitStatus health ordering for one terminal seat."""
    own = _RED_FIELD_IDS if self_faction == _FACTION_RED else _BLUE_FIELD_IDS
    enemy = _BLUE_FIELD_IDS if self_faction == _FACTION_RED else _RED_FIELD_IDS
    return own + enemy


def radar_robot_ids(self_faction: int) -> tuple[int, ...]:
    """Return the official RadarInfoToClient ordering for one terminal seat."""
    own = _RED_FIELD_IDS if self_faction == _FACTION_RED else _BLUE_FIELD_IDS
    enemy = _BLUE_FIELD_IDS if self_faction == _FACTION_RED else _RED_FIELD_IDS
    return enemy + own


def radar_centimetres(value: float) -> int:
    """Round metres to the unsigned centimetre protocol representation."""
    return max(0, int(round(value * 100)))


def dart_blind_duration(fixed_hit_number: int, target_type: int) -> float:
    """规则 5.7.3 的遮挡时长。fixed_hit_number 是本局这一方第几次命中前三类目标（从 1 起）；
    随机移动、末端移动目标每次 10 秒。第 5 次及以后手册没写，和终端一样按 2 秒算。"""
    if target_type >= 4:
        return 10.0
    return _DART_HIT_DURATIONS[min(max(fixed_hit_number - 1, 0), 3)]


def sentry_command_cost(command_id: int, match_elapsed: float, automatic: bool) -> int:
    """规则 5.7.2 + 表 5-7:指挥手一条哨兵指令的总价，和终端 sentry_cost() 一致。

    自动哨兵每次另加 50 干预费；哨兵不适用经验体系，立即复活按 1 级算；补血点补弹协议不带数量，只收干预费。
    """
    fee = 50 if automatic else 0
    remaining = max(0.0, rules.MATCH_SEC - match_elapsed)
    if command_id == 3:
        return 150 + fee
    if command_id == 4:
        return rules.remote_heal_cost(remaining) + fee
    if command_id == 6:
        return rules.pay_revive_cost(remaining, 1) + fee
    return fee


def zone_event(index: int, state: int) -> tuple[int, str] | None:
    """阵地易手的 Event（2.2.7 的 5–8）。index 按 _ZONE_NAMES，state 是绝对编码。

    前哨阵地的参数就是绝对编码 1–4；基地阵地只有两种状态：R 基地 1 红方完全 / 2 蓝方临时，
    B 基地 1 蓝方完全 / 2 红方临时。"""
    if index in (0, 1):
        return (5 + index, str(state)) if 1 <= state <= 4 else None
    if index == 2:
        return {_ZONE_RED_FULL: (7, "1"), _ZONE_BLUE_TEMP: (7, "2")}.get(state)
    if index == 3:
        return {_ZONE_BLUE_FULL: (8, "1"), _ZONE_RED_TEMP: (8, "2")}.get(state)
    return None


def base_status_code(defended: bool, armor_open: bool) -> int:
    """GlobalUnitStatus.base_status：0 有 100% 防御增益 1 无增益且护甲未展开 2 无增益且护甲展开。"""
    if defended:
        return 0
    return 2 if armor_open else 1


def max_assembly_difficulty(match_seconds: float) -> int:
    """表 5-11：开赛只能选一级，之后每满 1 分钟多解锁一级，3 分钟后一到四级都能选。"""
    return min(4, 1 + int(max(0.0, match_seconds) // 60))


def assembly_steps(difficulty: int) -> int:
    """表 5-10：一级只要放入，二级放入、平移，三级和四级放入、平移、旋转。"""
    return {1: 1, 2: 2}.get(difficulty, 3)


def assembly_ready(difficulty: int, elapsed: float) -> bool:
    """科技核心到位、这一级要的步骤都做完了，才能确认装配。"""
    return elapsed >= _ASSEMBLY_MOVE_SEC + _ASSEMBLY_STEP_SEC * assembly_steps(difficulty)


def assembly_timed_out(difficulty: int, elapsed: float) -> bool:
    if difficulty == 4:
        return elapsed - _ASSEMBLY_MOVE_SEC >= _ASSEMBLY_LEVEL4_SEC
    return elapsed >= _ASSEMBLY_TIMEOUT_SEC


def tech_core_state(difficulty: int | None, elapsed: float) -> dict[str, int]:
    """TechCoreMotionStateSync 字段 2–5、7、8。difficulty 为 None 表示没在装配。

    科技核心 1 初始位置、2 运动中、3 已到达；剩余时间只在四级装配中有效。"""
    if difficulty is None:
        return {"basic_state": 1, "putin_state": 0, "move_state": 0, "rotate_state": 0,
                "remain_time_all": 0, "remain_time_step": 0}
    arrived = elapsed >= _ASSEMBLY_MOVE_SEC
    since = max(0.0, elapsed - _ASSEMBLY_MOVE_SEC)
    done = min(assembly_steps(difficulty), int(since // _ASSEMBLY_STEP_SEC)) if arrived else 0
    level4 = difficulty == 4 and arrived
    return {
        "basic_state": 3 if arrived else 2,
        "putin_state": int(done >= 1), "move_state": int(done >= 2), "rotate_state": int(done >= 3),
        "remain_time_all": max(0, math.ceil(_ASSEMBLY_LEVEL4_SEC - since)) if level4 else 0,
        "remain_time_step": (math.ceil(_ASSEMBLY_STEP_SEC - since % _ASSEMBLY_STEP_SEC)
                             if level4 and done < 3 else 0),
    }


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
        # 重装装上 42mm 发射机构(launcher_ready)后,要打出一发被裁判系统测到才算「成功安装」(规则 5.7.1)。
        self.launcher_verified = _SELF_LOCAL_ID != 1
        self.team_ammo_bought = {17: 0, 42: 0}
        # 自机累计已发弹量(RobotDynamicStatus.total_projectiles_fired),每局清零。
        self.fired_total = 0
        # RobotInjuryStat(2.2.8):这一条命按原因累计的扣血,复活后清零。模拟器里飞镖以外的掉血都记成
        # 17mm 弹丸(随机掉血、控制台改血量)。
        self.injury = _new_injury()
        self._pending_dart = 0
        # 车站在能非远程兑换的地方(补给区、己方基地增益点、前哨站增益点,规则手册 5.4.2)时,
        # 服务器 1Hz 发 Buff 类型 6。默认站在上面,方便直接试兑换;控制台的开关模拟开出去。
        self.at_exchange_point = True
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
        # 两座前哨站现在归谁(下标 0 是开局在红方一侧的那座)。打掉重建后换主人(规则 5.6.1),
        # 只有对方能打它,所以控制台和随机掉血把它打到 0 时算主人的对手打的。
        self.outpost_owner = [_FACTION_RED, _FACTION_BLUE]
        # 规则 5.7.1 的 M、N:本局对方基地血量、控制度比自机这一方多出来的最大值。
        self.max_base_gap = 0
        self.max_control_gap = 0
        # 控制台按钮的结果(成功、拒绝原因、规则提醒),发在 sim/State 里给控制台显示。
        self.note = ""
        self.note_seq = 0
        self.red_economy = _INITIAL_ECONOMY
        self.blue_economy = _INITIAL_ECONOMY
        # 累计获得的经济（GlobalLogisticsStatus.total_economy_obtained），花钱不减。
        self.red_economy_total = _INITIAL_ECONOMY
        self.blue_economy_total = _INITIAL_ECONOMY
        # 累计总伤害由每次实际扣血累加（见 _deal_damage），不额外造随机数：面板
        # 同时显示血量差和总伤害，两者取自同一次扣血才不会互相矛盾。
        self.red_total_damage = 0
        self.blue_total_damage = 0
        # 堡垒（规则 5.6.4.9）：fortress_attack 是哪一方的堡垒正被对方激活（0 都没有）；
        # fortress_activation 是这一次激活了几秒，满 10 秒这一方的基地护甲展开。
        self.fortress_attack = 0
        self.fortress_activation = {_FACTION_RED: 0.0, _FACTION_BLUE: 0.0}
        # 默认不自动；`fortress auto` 才随机让一方去激活另一方的堡垒。
        self.fortress_auto = False
        self._fortress_last = 0.0
        # 基地护甲是否已展开：堡垒被激活满 10 秒，或飞镖条件满足（规则 5.7.3）。
        self.armor_open = {_FACTION_RED: False, _FACTION_BLUE: False}
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
        # 控制台强制雷达标记的敌方车号(0 = 不强制)。
        self.radar_lock_override = 0
        self.vision_online = True
        # 5.5.2：步兵不选时默认底盘「血量优先」、发射机构「冷却优先」(枚举都是 1)。
        self.chassis_mode = 1
        self.shooter_mode = 1
        self.autoaim = True
        # 待发的 Event（协议 2.2.7）：(event_id, param)。只放协议里有编号的事件。
        self.events: list[tuple[int, str]] = []
        # 致盲按「哪一方被遮挡」建模：0未致盲 1红方 2蓝方。终端收不到这些，只收 Event 9，
        # 遮挡时长自己按规则算；这里的账本给模拟器日志和控制面板用。
        self.blind_side = 0
        self.blind_target_type = 0
        self.blind_remaining_ms = 0
        self._blind_window: tuple[float, float, int, int] | None = None
        self.blind_started = {_FACTION_RED: -1.0, _FACTION_BLUE: -1.0}
        self.blind_until = {_FACTION_RED: -1.0, _FACTION_BLUE: -1.0}
        self.blind_type = {_FACTION_RED: 1, _FACTION_BLUE: 1}
        # 按命中方计：本局命中前三类目标几次（决定遮挡时长），命中基地固定类目标几次（满 4 次护甲展开）。
        self.dart_fixed_hits = {_FACTION_RED: 0, _FACTION_BLUE: 0}
        self.dart_base_hits = {_FACTION_RED: 0, _FACTION_BLUE: 0}
        self._auto_blind_fired = 0
        self.dart = {side: _new_dart_state() for side in (_FACTION_RED, _FACTION_BLUE)}
        # 指挥手在终端上选中的飞镖目标（DartSelectTargetStatusSync.target_id）。
        self.dart_target = 1
        self.tech_level = {_FACTION_RED: 0, _FACTION_BLUE: 0}
        # 重装正在进行的装配：难度、开始的比赛秒。
        self.assembly: dict[str, float | int] | None = None
        # 重装站在己方装配区增益点上（规则手册 5.6.4.7）。终端收不到这一项（只在发给车的 0x0209 里）；
        # 默认站在上面，方便直接试装配，控制台的开关模拟开出去。
        self.in_assembly_zone = True
        self.sentry_posture = 1
        self.sentry_powered = False
        self.sentry_last_switch = -99.0
        self.sentry_auto = True
        self.last_map_click = -1.0
        self.last_periodic_mark = -1e9
        self._base_attacked_at = -1e9
        # 我方能量机关：1 未激活 2 正在激活 3 已激活；rune_until 是当前状态结束的比赛秒。
        self.rune_status = 1
        self.rune_arms = 0
        self.rune_until = 0.0
        self.rune_big = False
        self.rune_used = {"small": 0, "big": 0}
        # 自机的地形跨越增益:Buff 类型 -> (增益值, 结束的比赛秒, 最大时长)。阵亡清掉(5.6.4.5)。
        self.terrain: dict[int, tuple[int, float, float]] = {}
        # 拿到过哪几种地形增益,各自到什么时候:判断「又拿到另一种」用。
        self.terrain_kinds: dict[int, float] = {}
        self.terrain_pre_until = 0.0
        # 远程兑换 6 秒后才到账:(到账的比赛秒, "ammo"/"heal", 发数, 下单时的阵亡次数)。
        self.remote_pending: list[tuple[float, str, int, int]] = []
        self.deaths = 0
        # 脱战(规则术语表):存活且连续 6 秒没发射、没掉血。None = 本局还没交过火。
        self._combat_at: float | None = None
        self._hp_seen = self.hp
        # 终端发来的键鼠帧:(到达时刻, 帧),只留最近一秒多,给控制面板算帧率和位移。
        self.km_frames: deque = deque(maxlen=200)
        # 己方其他车的剩余允许发弹量（GlobalUnitStatus.robot_bullets 只报己方）。
        self.other_ammo = {rid: (rules.SENTRY_INITIAL_AMMO if rid % 100 == 7 else 0)
                           for rid in _FRIENDLY_IDS}
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
        # 结算没走完就点了准备/自检/倒计时:先把下一局正式开起来。不然局数不涨、上一局的结果
        # 一直挂着,下一局打爆基地也判不出胜负,结算报的还是上一局。整场已分出胜负就从第一局重来。
        if self.match_over:
            self.reset()
        else:
            with self._round_lock:
                if self.round_end_reason:
                    self.start_next_round()
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
        return self.red_base_hp if _is_red(robot_id) else self.blue_base_hp

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
            self.deaths += 1
            # 规则 5.1.3/5.3.2：死亡清热量和临时锁，永久过热锁仍随本局。
            self.heat = 0.0
            self.heat_locked = False

    def _revive(self, robot_id: int, paid: bool) -> None:
        state = self.respawn_state[robot_id]
        now = self.match_elapsed() or 0.0
        fraction = rules.PAY_REVIVE_HP_FRACTION if paid else rules.REVIVE_HP_FRACTION
        maximum = self.max_hp if robot_id == SELF_ROBOT_ID else self.other_max_hp[robot_id]
        self._write_hp(robot_id, max(1, rules.round_half_up(maximum * fraction)))
        if robot_id == SELF_ROBOT_ID:
            self.injury = _new_injury()
            self._pending_dart = 0
        duration = rules.PAY_REVIVE_WEAK_SEC if paid else rules.REVIVE_INVINCIBLE_SEC
        state.update(dead=False, progress=0.0, total=0, weak=True,
                     weak_until=now + duration, invincible_until=now + duration,
                     revived_at=now, revived_paid=paid)

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
            state["progress"] = min(state["progress"] + rate * delta, float(state["total"]))
            # 协议 2.2.25:读条完成后要发「确认复活」才复活。自机等终端(或控制面板)确认;
            # 别的车没有人替它们点,读条满就自己复活。
            if state["progress"] >= state["total"] and robot_id != SELF_ROBOT_ID:
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

    def confirm_revive(self) -> str | None:
        """确认复活(CommonCommand 3)：读条完成才生效。成功返回 None，失败返回原因。"""
        if self.match_elapsed() is None:
            return "比赛未开始或已结束"
        state = self.respawn_state.get(SELF_ROBOT_ID)
        if state is None or not self._is_ground(SELF_ROBOT_ID):
            return "该机器人不适用复活"
        if not state["dead"]:
            return "机器人未阵亡"
        if state["progress"] < state["total"]:
            return f"读条未完成 {int(state['progress'])}/{state['total']}"
        self._revive(SELF_ROBOT_ID, paid=False)
        return None

    def out_of_combat(self) -> bool:
        """阵亡不算脱战;比赛开始时默认脱战,赛前也没有交战可言。"""
        if self.hp == 0:
            return False
        me = self.match_elapsed()
        if me is None or self._combat_at is None:
            return True
        return me - self._combat_at >= rules.OUT_OF_COMBAT_SEC

    def out_of_combat_countdown(self) -> int:
        """离脱战还有几秒。阵亡、已脱战或没交过火都是 0(协议字段是 uint32,不能为负)。"""
        me = self.match_elapsed()
        if me is None or self.hp == 0 or self._combat_at is None:
            return 0
        return max(0, int(math.ceil(rules.OUT_OF_COMBAT_SEC - (me - self._combat_at))))

    def _remote_check(self) -> str | None:
        if self.match_elapsed() is None:
            return "比赛未开始或已结束"
        if self.hp == 0:
            return "机器人已阵亡"
        if not self._is_ground(SELF_ROBOT_ID):
            return "只有地面机器人能远程兑换"
        if not self.out_of_combat():
            return f"未脱战，还要 {self.out_of_combat_countdown()} 秒"
        return None

    def remote_buy_ammo(self, rounds: int) -> str | None:
        """远程兑换允许发弹量(5.4.2)：脱战时花金币，6 秒后到账。"""
        error = self._remote_check()
        if error:
            return error
        if not self.launcher_ready:
            return "未安装发射机构"
        cost = rules.ammo_exchange_cost(self.caliber, rounds, remote=True)
        if cost is None:
            unit = rules.AMMO_EXCHANGE[(self.caliber, True)][0]
            return f"发数必须是 {unit} 的整数倍"
        if self.team_ammo_bought[self.caliber] + rounds > rules.AMMO_CAP[self.caliber]:
            return f"超出全队 {self.caliber}mm 兑换上限"
        if self.coin < cost:
            return f"金币不足，需要 {cost}"
        self._spend_coins(cost)
        self.team_ammo_bought[self.caliber] += rounds
        self.remote_pending.append((self.match_elapsed() + rules.REMOTE_AMMO_DELAY_SEC, "ammo", rounds,
                                    self.deaths))
        return None

    def remote_heal(self) -> str | None:
        """远程兑换血量(5.3.1)：脱战时花金币，6 秒后加上限血量的 60%×λ；6 秒内阵亡作废、不退钱。"""
        error = self._remote_check()
        if error:
            return error
        cost = rules.remote_heal_cost(max(0.0, rules.MATCH_SEC - self.match_elapsed()))
        if self.coin < cost:
            return f"金币不足，需要 {cost}"
        self._spend_coins(cost)
        self.remote_pending.append((self.match_elapsed() + rules.REMOTE_HEAL_DELAY_SEC, "heal", 0,
                                    self.deaths))
        return None

    def tick_remote(self) -> None:
        """到点的远程兑换入账；比赛不在进行时不推进。"""
        me = self.match_elapsed()
        if me is None or not self.remote_pending:
            return
        due = [item for item in self.remote_pending if item[0] <= me]
        self.remote_pending = [item for item in self.remote_pending if item[0] > me]
        for _at, kind, rounds, deaths in due:
            if kind == "ammo":
                self.ammo += rounds
                print(f"[server] 远程兑换的 {rounds} 发到账，剩余允许发弹量 {self.ammo}")
            elif deaths != self.deaths or self.hp == 0:
                print("[server] 远程兑换血量作废：6 秒内阵亡")
            else:
                gained = rules.remote_heal_amount(self.max_hp, rules.logistics_lambda(
                    self._friendly_base_hp(SELF_ROBOT_ID)))
                before = self.hp
                self.hp = min(self.max_hp, self.hp + gained)
                self._hp_seen = self.hp
                print(f"[server] 远程兑换血量到账 +{self.hp - before}，HP={self.hp}")

    def track_combat(self) -> None:
        """掉血算交战。发射在 shoot() 里记。"""
        me = self.match_elapsed()
        if me is not None and self.hp < self._hp_seen:
            self._combat_at = me
            lost = self._hp_seen - self.hp
            dart = min(lost, self._pending_dart)
            self._pending_dart -= dart
            self.injury["dart"] += dart
            self.injury["small"] += lost - dart
        self._hp_seen = self.hp

    def select_performance(self, shooter: int | None, chassis: int | None) -> str | None:
        """5.5.2：步兵在准备阶段开始后、七分钟比赛开始前选底盘和发射机构。"""
        if _SELF_LOCAL_ID not in (3, 4):
            return "只有步兵有性能体系"
        if self.stage()[0] not in (_STAGE_PREP, _STAGE_CHECK, _STAGE_COUNTDOWN):
            return "只能在准备阶段开始后、比赛开始前选"
        if shooter not in (None, 1, 2) or chassis not in (None, 1, 2):
            return "枚举值只能是 1 或 2"
        if shooter is not None:
            self.shooter_mode = shooter
        if chassis is not None:
            self.chassis_mode = chassis
        return None

    def rune_chances(self) -> int:
        """当前阶段(小/大能量机关)还剩几次激活机会。"""
        me = self.match_elapsed()
        if me is None:
            return 0
        if me < 180.0:
            return sum(1 for at in _SMALL_RUNE_AT if me >= at) - self.rune_used["small"]
        return sum(1 for at in _BIG_RUNE_AT if me >= at) - self.rune_used["big"]

    def enemy_rune(self, big: bool) -> str | None:
        """敌方激活能量机关:只发 Event 3/4(两条事件都不带是哪一方的),我方能量机关状态不变。"""
        me = self.match_elapsed()
        if me is None:
            return "比赛未开始或已结束"
        if big:
            self.events.append((3, _ENEMY_BIG_RUNE_EVENT))
        self.events.append((4, "2" if big else "1"))
        return None

    def gain_terrain(self, kind: int) -> str | None:
        """自机拿到一种地形跨越增益(5.6.4.5)。已经拿着另一种时防御变成 50%,时长取两者较大。"""
        me = self.match_elapsed()
        if me is None:
            return "比赛未开始或已结束"
        if self.hp == 0:
            return "自机阵亡"
        if kind == _TERRAIN_PRE_HIGHLAND:
            self.terrain_pre_until = me + 5.0
            return None
        if kind not in _TERRAIN:
            return f"没有编号 {kind} 的地形增益"
        _name, buff_type, level, seconds = _TERRAIN[kind]
        others = [until for held, until in self.terrain_kinds.items() if held != kind and until > me]
        self.terrain_kinds[kind] = me + seconds
        self.terrain_pre_until = 0.0
        if buff_type == 3:
            self.terrain[3] = (0, me + seconds, seconds)
        if others:
            until = max(max(others), me + seconds)
            self.terrain[2] = (50, until, until - me)
        elif buff_type == 2:
            self.terrain[2] = (level, me + seconds, seconds)
        return None

    def self_buffs(self) -> list[tuple[int, int, int, int]]:
        """这一秒要发给终端的自机 Buff(2.2.14):(类型, 增益值, 最大时长, 剩余时长),一个来源一条。"""
        out: list[tuple[int, int, int, int]] = []
        me = self.match_elapsed()
        if self.at_exchange_point:
            out.append((6, 0, 0, 0))
        if self.hp == 0:
            self.terrain.clear()
            self.terrain_kinds.clear()
            self.terrain_pre_until = 0.0
        if me is None or self.hp == 0:
            return out
        left = lambda until: max(0, int(math.ceil(until - me)))
        if self.rune_status == 3 and self.rune_until > me:
            if self.rune_big:
                for buff_type, level in _BIG_RUNE_BUFFS:
                    out.append((buff_type, level, int(_BIG_RUNE_ACTIVE_SEC), left(self.rune_until)))
                out.append((3, int(round(self.cooling_rate * (_BIG_RUNE_COOLING_TIMES - 1))),
                            int(_BIG_RUNE_ACTIVE_SEC), left(self.rune_until)))
            else:
                out.append((2, 25, int(_RUNE_ACTIVE_SEC), left(self.rune_until)))
        if _SELF_LOCAL_ID == 1 and me < _HERO_EARLY_SEC and not self.launcher_verified:
            out.append((2, 50, int(_HERO_EARLY_SEC), left(_HERO_EARLY_SEC)))
        for buff_type, (level, until, longest) in self.terrain.items():
            if until <= me:
                continue
            if buff_type == 3:
                level = int(round(self.cooling_rate * 0.5))
            out.append((buff_type, level, int(longest), left(until)))
        if self.terrain_pre_until > me:
            out.append((7, 0, 5, left(self.terrain_pre_until)))
        return out

    def note_km(self, frame) -> None:
        self.km_frames.append((time.monotonic(), frame))

    def km_summary(self) -> dict | None:
        """最近一秒的键鼠帧：帧率、此刻按着的键和鼠标键、这一秒鼠标总位移。"""
        now = time.monotonic()
        recent = [f for t, f in self.km_frames if now - t <= 1.0]
        if not recent:
            return None
        last = recent[-1]
        names = "W S A D Shift Ctrl Q E R F G Z X C V B".split()
        keys = [names[bit] for bit in range(16) if last.keyboard_value & (1 << bit)]
        buttons = [name for name, down in (("左", last.left_button_down), ("右", last.right_button_down),
                                           ("中", last.mid_button_down)) if down]
        return {"hz": len(recent), "keys": " ".join(keys), "buttons": " ".join(buttons),
                "mouse": [sum(f.mouse_x for f in recent), sum(f.mouse_y for f in recent),
                          sum(f.mouse_z for f in recent)]}

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
        """RobotRespawnStatus 字段；读条满后 can_free 一直为真，直到确认复活。"""
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
        """手动设置阵地状态。返回 False 表示阵地序号或状态值非法。

        前哨阵地设成某一方完全控制,而前哨站不是这一方的:按规则只能是前哨站被打掉、
        重建成这一方的 500 血(5.6.1、表 5-21),所以按这个结果摆,不只改字。
        """
        if not 0 <= index < len(self.zones) or not 0 <= state < len(_ZONE_STATE_NAMES):
            return False
        if state not in _ZONE_ALLOWED_STATES[index]:
            return False
        before = self.zones[index]
        if state in (_ZONE_RED_TEMP, _ZONE_BLUE_TEMP):
            side = _FACTION_RED if state == _ZONE_RED_TEMP else _FACTION_BLUE
            if self.zones[index] == _zone_full(side):
                return True
            if index < 2 and self.outpost_owner[index] == side and self._outpost_hp(index) > 0:
                self.say(f"{_ZONE_NAMES[index]}的前哨站还在、归{_SIDE_NAMES[side]}:要先被对方打掉前哨站,"
                         f"{_SIDE_NAMES[side]}才会变成临时控制(建筑血量设 0)")
                return False
            if index < 2 and self.outpost_owner[index] != side and self._outpost_hp(index) == 0:
                owner = self.outpost_owner[index]
                self.say(f"{_ZONE_NAMES[index]}的前哨站已被{_SIDE_NAMES[side]}打掉:{_SIDE_NAMES[owner]} 5 秒不激活就转成"
                         f"{_SIDE_NAMES[side]}完全控制,要直接摆这个结果请选「{_ZONE_STATE_NAMES[_zone_full(side)]}」")
                return False
            self.zones[index] = state
            self.zone_progress[index] = _ZONE_PROGRESS_SEC
        else:
            side = _FACTION_RED if state == _ZONE_RED_FULL else _FACTION_BLUE
            if index < 2 and self.outpost_owner[index] != side and self._outpost_hp(index) > 0:
                self.events.append((2, _OUTPOST_EVENT_IDS[index]))
                self.say(f"{_ZONE_NAMES[index]}改成{_SIDE_NAMES[side]}完全控制:按规则前哨站先被打掉,"
                         f"再重建成{_SIDE_NAMES[side]}的,500 血")
                self._rebuild_outpost(index, side)
            elif index < 2 and self._outpost_hp(index) == 0:
                self._rebuild_outpost(index, side)
            self.zones[index] = state
            self.zone_progress[index] = 0.0
        self._zone_changed(index, before)
        return True

    def say(self, text: str) -> None:
        """控制台按钮的结果,经 sim/State 显示在控制台底部。"""
        self.note = text
        self.note_seq += 1

    def _outpost_hp(self, index: int) -> int:
        return self.red_outpost_hp if index == 0 else self.blue_outpost_hp

    def _set_outpost_hp(self, index: int, hp: int) -> None:
        if index == 0:
            self.red_outpost_hp = hp
        else:
            self.blue_outpost_hp = hp

    def _rebuild_outpost(self, index: int, side: int) -> None:
        """规则 5.6.1:前哨站重建成 side 拥有的,500 血。"""
        self.outpost_owner[index] = side
        self._set_outpost_hp(index, _OUTPOST_REBUILD_HP)

    def _zone_changed(self, index: int, before: int) -> None:
        """阵地状态真的变了才发 Event 5–8:持续激活会反复写同一个临时控制。"""
        if self.zones[index] == before:
            return
        event = zone_event(index, self.zones[index])
        if event is not None:
            self.events.append(event)

    def _destroy_outpost(self, index: int) -> None:
        """前哨站被击毁：按表 5-21 转化阵地控制，按 5.6.1 重建。

        只有前哨站主人的对手能打它。对手已经临时控制这块阵地时,打掉就转为对手完全控制,
        前哨站当场重建成对手的。主人完全控制时被打掉,降为主人临时控制;主人 5 秒不激活,
        对手完全控制并重建(见 _expire_temp_control)。
        """
        state = self.zones[index]
        attacker = _opponent(self.outpost_owner[index])
        # Event 2:参数是被击毁的前哨站编号，R 前哨 11、B 前哨 111（附录二，按开局位置）。
        self.events.append((2, _OUTPOST_EVENT_IDS[index]))
        if state in (_zone_temp(attacker), _zone_full(attacker)):
            self.zones[index] = _zone_full(attacker)
            self.zone_progress[index] = 0.0
            self._rebuild_outpost(index, attacker)
        else:
            if state == _zone_full(self.outpost_owner[index]):
                self.zones[index] = _zone_temp(self.outpost_owner[index])
            self.zone_progress[index] = _ZONE_PROGRESS_SEC
            self._set_outpost_hp(index, 0)
        self._zone_changed(index, state)

    def set_structure_hp(self, index: int, hp: int) -> bool:
        """手动设置基地/前哨站血量。返回 False 表示建筑序号非法。

        前哨站设为 0 必须走击毁流程,否则只改数字会让它永远停在 0,
        既不转化阵地也不重建——面板上看起来就是「打爆了但没反应」。
        基地有 100% 防御时照样改(摆测试局面要用),但在控制台提醒这不是车打的。
        """
        if not 0 <= index < len(_STRUCTURE_NAMES):
            return False
        hp = max(0, min(_STRUCTURE_MAX_HP[index], hp))
        if index in (0, 1):
            side = _FACTION_RED if index == 0 else _FACTION_BLUE
            current = self.red_base_hp if index == 0 else self.blue_base_hp
            if hp < current and self._base_defended(side):
                self.say(f"{_SIDE_NAMES[side]}前哨阵地还是{_SIDE_NAMES[side]}完全控制,基地有 100% 防御:"
                         "按规则车打不动,这样掉血只能算飞镖或判罚")
            self._note_base_damage(side, current - hp)
            if index == 0:
                self.red_base_hp = hp
            else:
                self.blue_base_hp = hp
            if self.match_elapsed() is not None:
                self.track_gaps()
        else:
            outpost = index - 2
            if hp == 0 and self._outpost_hp(outpost) > 0:
                self._destroy_outpost(outpost)
            elif hp > 0 and self._outpost_hp(outpost) == 0:
                self.say(f"{_STRUCTURE_NAMES[index]}已被打掉:要等阵地转化后重建(500 血),"
                         "不能直接加血;可以直接把阵地设成某一方完全控制")
                return False
            else:
                self._set_outpost_hp(outpost, hp)
        # 手动把基地打到 0 也要结束本局,和真实伤害走同一条判定。
        if index in (0, 1) and hp == 0 and self.match_elapsed() is not None:
            self._end_round_by_base_hp()
        return True

    def set_fortress(self, side: int) -> bool:
        """指定哪一方的堡垒正被对方激活：1 红方 / 2 蓝方 / 0 都没有，3 表示随机。"""
        if side == 3:
            self.fortress_auto = True
            return True
        if side not in (0, 1, 2):
            return False
        self.fortress_auto = False
        self._set_fortress_attack(side)
        return True

    def _set_fortress_attack(self, side: int) -> None:
        # 换了激活对象就从 0 重新计：规则按「单次激活时间」满 10 秒算。
        if side != self.fortress_attack:
            self.fortress_activation = {_FACTION_RED: 0.0, _FACTION_BLUE: 0.0}
        self.fortress_attack = side

    def _open_armor(self, side: int) -> None:
        if self.armor_open[side]:
            return
        self.armor_open[side] = True
        # Event 13 只报「对方基地护甲展开」。
        if side == _ENEMY_FACTION:
            self.events.append((13, ""))

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
        return None

    def trigger_blind(self, side: int, target_type: int) -> str | None:
        """让 side 一方（被命中、被遮挡的一方）中一发飞镖。成功返回 None，否则返回原因。

        直接记一次命中，不走闸门和冷却：手动注入和时刻表要的就是「这一刻中镖」。
        扣血和开闸发射命中完全一样（_apply_dart_damage）；飞镖只在比赛中发射，赛外不扣血。
        比赛中这一发也用掉命中方本局的一枚飞镖（规则 5.7.3 每局 4 枚），和开闸发射共用。
        """
        if side not in (_FACTION_RED, _FACTION_BLUE):
            return "阵营必须是 1(红) 或 2(蓝)"
        if not 1 <= target_type <= len(_DART_TARGET_NAMES):
            return f"目标类型必须是 1-{len(_DART_TARGET_NAMES)}"
        hitter = _opponent(side)
        if self.match_elapsed() is not None:
            state = self.dart[hitter]
            if state["used"] >= _DART_MAX_PER_ROUND:
                return f"{_SIDE_NAMES[hitter]}本局飞镖已用完"
            state["used"] += 1
            self._apply_dart_damage(hitter, target_type)
        self._register_dart_hit(hitter, target_type)
        return None

    def _apply_dart_damage(self, hitter: int, target_type: int) -> int:
        """按表 5-2 和 5.7.3 结算一发飞镖命中的扣血，返回打在前哨站或基地上的实际伤害。

        前哨站 1000；基地固定目标 200、随机固定 300、随机移动 800、末端移动 1250，
        飞镖扣血不吃防御增益。移动类目标另外让对方存活的地面机器人各扣自己上限血量的
        20% / 50%（空中机器人和无敌的除外）。
        """
        victim = _FACTION_BLUE if hitter == _FACTION_RED else _FACTION_RED
        damage = _DART_DAMAGE[target_type - 1]
        if target_type == 1:
            # 打的是对方拥有的前哨站;两座都是对方的时候先打开局在对方一侧的那座。
            home = 0 if victim == _FACTION_RED else 1
            owned = [i for i in (home, 1 - home) if self.outpost_owner[i] == victim]
            if not owned:
                return 0
            index = owned[0]
            applied = min(self._outpost_hp(index), damage)
            self._set_outpost_hp(index, self._outpost_hp(index) - applied)
            # 已经倒下的前哨站再中镖不扣血，也不能再走一遍击毁流程。
            if applied and self._outpost_hp(index) == 0:
                self._destroy_outpost(index)
            self._deal_damage(victim == _FACTION_RED, applied)
            return applied
        dealt = self._damage_structure(victim, damage, bypass_defense=True)
        fraction = _DART_ROBOT_HP_FRACTION[target_type - 1]
        if fraction:
            if (victim == _SELF_FACTION and self.hp > 0
                    and not self.is_invincible(SELF_ROBOT_ID)):
                applied = min(self.hp, int(self.max_hp * fraction))
                self.hp -= applied
                self._pending_dart += applied
                self._deal_damage(True, applied)
                if self.hp == 0:
                    self._sync_respawn(SELF_ROBOT_ID)
                    self._register_kill(SELF_ROBOT_ID, _DART_ROBOT_ID[hitter])
            for rid, hp in self.other_hp.items():
                # 规则 5.7.3：移动目标只对存活的地面机器人扣血，空中机器人除外。
                if (hp > 0 and rid % 100 != 6 and not self.is_invincible(rid)
                        and (_is_red(rid)) == (victim == _FACTION_RED)):
                    applied = min(hp, int(self.other_max_hp[rid] * fraction))
                    self.other_hp[rid] = hp - applied
                    self._deal_damage(victim == _FACTION_RED, applied)
                    if self.other_hp[rid] == 0:
                        self._sync_respawn(rid)
                        self._register_kill(rid, _DART_ROBOT_ID[hitter])
        return dealt

    def _register_dart_hit(self, hitter: int, target_type: int) -> None:
        """飞镖命中的共同后果：遮挡、护甲、Event 9。伤害由调用方按目标结算。"""
        victim = _FACTION_BLUE if hitter == _FACTION_RED else _FACTION_RED
        if target_type <= 3:
            self.dart_fixed_hits[hitter] += 1
        self._apply_blind(victim, target_type,
                          dart_blind_duration(self.dart_fixed_hits[hitter], target_type))
        if target_type in (2, 3):
            self.dart_base_hits[hitter] += 1
        # 规则 5.7.3:固定类目标 4 发全中基地,或移动类目标任意一发命中基地,护甲展开。
        if target_type >= 4 or self.dart_base_hits[hitter] >= 4:
            self._open_armor(victim)
        self.events.append((9, f"{hitter},{target_type}"))

    def _apply_blind(self, side: int, target_type: int, duration: float) -> None:
        me = self.match_elapsed()
        if me is None:
            return
        if self.blind_until[side] <= me:
            self.blind_started[side] = me
        self.blind_until[side] = max(me, self.blind_until[side]) + duration
        self.blind_type[side] = target_type

    def clear_blind(self) -> None:
        """清掉模拟器自己的遮挡账本。终端按 Event 9 自己算，清不掉终端那边。"""
        self.blind_started = {_FACTION_RED: -1.0, _FACTION_BLUE: -1.0}
        self.blind_until = {_FACTION_RED: -1.0, _FACTION_BLUE: -1.0}
        self._blind_window = None
        self.blind_side = 0
        self.blind_target_type = 0
        self.blind_remaining_ms = 0

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
        self._note_base_damage(side, applied)
        if self.red_base_hp == 0 or self.blue_base_hp == 0:
            self._end_round_by_base_hp()
        return applied

    def _note_base_damage(self, side: int, applied: int) -> None:
        """我方基地掉血时发 Event 11,协议规定每次触发有 5 秒内置冷却。"""
        if side != _SELF_FACTION or applied <= 0:
            return
        now = self.elapsed
        if now - self._base_attacked_at >= 5.0:
            self._base_attacked_at = now
            self.events.append((11, ""))

    def dart_open(self, side: int) -> str | None:
        """开闸(规则 5.7.3):比赛 30 秒、4 分钟各给 1 次机会,没用的留着;闸门关上后冷却 15 秒才能再开。
        成功返回 None。"""
        if side not in self.dart:
            return "阵营必须是 1(红) 或 2(蓝)"
        me = self.match_elapsed()
        if me is None:
            return "比赛未开始"
        state = self.dart[side]
        if state["used"] >= _DART_MAX_PER_ROUND:
            return "本局飞镖已用完"
        if state["open_at"] >= 0 and me <= state["close_at"]:
            return "闸门已经开着"
        if state["openings"] >= sum(1 for unlock in _DART_UNLOCK_SEC if me >= unlock):
            later = [unlock for unlock in _DART_UNLOCK_SEC if unlock > me]
            return f"{later[0] - me:.0f} 秒后才有开闸机会" if later else "飞镖闸门开启次数已达上限"
        if state["close_at"] >= 0 and me < state["close_at"] + _DART_GATE_COOLDOWN_SEC:
            return f"飞镖发射站冷却中，剩余 {state['close_at'] + _DART_GATE_COOLDOWN_SEC - me:.1f}s"
        state["openings"] += 1
        state["open_at"] = me
        state["close_at"] = me + _DART_GATE_OPEN_SEC + _DART_FIRE_WINDOW_SEC
        # Event 10 只报「对方飞镖闸门开启」。
        if side == _ENEMY_FACTION:
            self.events.append((10, ""))
        return None

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
        last = state["last_hit"].get(target_type, -1e9)
        if me - last < _DART_TARGET_COOLDOWN_SEC:
            return "同一目标 2 秒内不能重复命中"
        state["used"] += 1
        state["last_hit"][target_type] = me
        dealt = self._apply_dart_damage(side, target_type)
        self._register_dart_hit(side, target_type)
        print(f"[server] {'红方' if side == _FACTION_RED else '蓝方'}飞镖命中"
              f"{_DART_TARGET_NAMES[target_type - 1]}，{dealt} 伤害", flush=True)
        return None

    def buy_ammo(self, caliber: int, rounds: int) -> str | None:
        """非远程兑换允许发弹量(5.4.2)：车要站在补给区、己方基地增益点或前哨站增益点上。"""
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
        if not self.at_exchange_point:
            return "未在兑换点"
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
        # 重装装上后打出的第一发就是验证弹(规则 5.7.1),不打到人、不算伤害。
        if not self.launcher_verified:
            error = self.verify_launcher()
            if error is not None:
                return error
            count -= 1
            if count == 0:
                return None
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
        self.fired_total += fired
        self._combat_at = self.match_elapsed()
        total = min(hp, damage)
        self.other_hp[target] = hp - total
        self._sync_respawn(target)
        victim_is_red = _is_red(target)
        if victim_is_red:
            self.blue_total_damage += total
            self.blue_hits += fired
        else:
            self.red_total_damage += total
            self.red_hits += fired
        if self.other_hp[target] == 0:
            self._register_kill(target, SELF_ROBOT_ID)
        return None

    def shooter_modules(self) -> tuple[int, int]:
        """RobotModuleStatus 的 17mm、42mm 测速模块(2.2.12,0 离线 1 在线):装着哪种发射机构亮哪个。"""
        if not self.launcher_ready:
            return 0, 0
        return (1, 0) if self.caliber == 17 else (0, 1)

    def track_gaps(self) -> None:
        """记下规则 5.7.1 的 M、N:本局对方基地血量、控制度比自机这一方多出来的最大值。"""
        red = _SELF_FACTION == _FACTION_RED
        own_base, enemy_base = ((self.red_base_hp, self.blue_base_hp) if red
                                else (self.blue_base_hp, self.red_base_hp))
        own_cp, enemy_cp = ((self.red_control_points, self.blue_control_points) if red
                            else (self.blue_control_points, self.red_control_points))
        self.max_base_gap = max(self.max_base_gap, enemy_base - own_base)
        self.max_control_gap = max(self.max_control_gap, enemy_cp - own_cp)

    def launcher_unlock_sec(self) -> float:
        """规则 5.7.1:比赛进行 300 − M/50 − 2N/3 秒后解锁 42mm 安装权限。"""
        return max(0.0, 300.0 - self.max_base_gap / 50.0 - 2.0 * self.max_control_gap / 3.0)

    def launcher_permission(self) -> bool:
        """RobotModuleStatus 字段 11(42mm 安装权限,规则手册 5.7.1):完成三级装配,或到了解锁时间。
        已经装上的当然有权限。"""
        if self.tech_level[_SELF_FACTION] >= 3:
            return True
        me = self.match_elapsed()
        if me is not None and me >= self.launcher_unlock_sec():
            return True
        return _SELF_LOCAL_ID == 1 and self.launcher_ready

    def set_module(self) -> str | None:
        """控制台「装42mm」:在工作台装上模块化 42mm 发射机构。装上后发射机构上电,能买 42mm 弹;
        要再打出一发验证弹才算成功安装(verify_launcher)。"""
        if _SELF_LOCAL_ID != 1:
            return "只有重装机器人可安装模块化 42mm 发射机构"
        if self.launcher_ready:
            return ("模块化 42mm 发射机构已经成功安装" if self.launcher_verified
                    else "已经装上了,还差打一发验证弹")
        me = self.match_elapsed()
        if me is None:
            return "比赛未开始或已结束"
        if self.hp == 0:
            return "机器人已阵亡"
        if not self.launcher_permission():
            return (f"还没有安装权限:要装成三级装配,或等到比赛第 {self.launcher_unlock_sec():.0f} 秒"
                    f"(规则 5.7.1,基地血量、控制度落后越多越早),现在第 {me:.0f} 秒")
        self.launcher_ready = True
        self.caliber = 42
        self.heat_limit = rules.HEAVY_HEAT_LIMIT
        self.cooling_rate = rules.HEAVY_COOLING
        self.say("重装装上了 42mm 发射机构:现在能买 42mm 弹;打出一发验证弹后才算成功安装")
        return None

    def verify_launcher(self) -> str | None:
        """装上后打出的第一发:被裁判系统测到,重装才算成功安装(规则 5.7.1,发 Event 16)。

        这一发不计热量、初速和伤害。手册没说它不扣允许发弹量,按 5.4.2「每发射 1 发减 1」照扣;
        允许发弹量是 0 时打出算超发,模拟器不模拟超发后的 42mm 屏蔽,只提醒。"""
        if _SELF_LOCAL_ID != 1:
            return "只有重装机器人可安装模块化 42mm 发射机构"
        if not self.launcher_ready:
            return "还没装上 42mm 发射机构:先点「装42mm」"
        if self.launcher_verified:
            return "已经成功安装了,不用再打验证弹"
        me = self.match_elapsed()
        if me is None:
            return "比赛未开始或已结束"
        if self.hp == 0:
            return "机器人已阵亡"
        if self.is_weak(SELF_ROBOT_ID):
            return "复活虚弱中，发射机构锁定"
        overshoot = self.ammo == 0
        if not overshoot:
            self.ammo -= 1
        self.fired_total += 1
        self._combat_at = me
        self.launcher_verified = True
        gained = rules.HEAVY_MAX_HP - self.max_hp
        self.max_hp = rules.HEAVY_MAX_HP
        if gained > 0:
            self.hp = min(self.max_hp, self.hp + gained)
        self.events.append((16, ""))
        self.say("验证弹被测到:重装成功安装 42mm 发射机构(Event 16),上限血量 600"
                 + (";允许发弹量是 0,按规则 5.4.2 这一发算超发" if overshoot else ",扣 1 发允许发弹量"))
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
        """时间耗尽时按规则 5.8 的顺序判定：控制度、总伤害、总剩余血量、平局。

        end_reason 照协议 2.2.3:2 控制度 3 全队总攻击伤害 4 全队总剩余血量 5 平局。
        """
        red_remain = sum(hp for rid, hp in self.other_hp.items() if _is_red(rid))
        blue_remain = sum(hp for rid, hp in self.other_hp.items() if not _is_red(rid))
        if _SELF_LOCAL_ID != 6:
            if _SELF_FACTION == _FACTION_RED:
                red_remain += self.hp
            else:
                blue_remain += self.hp
        for red, blue, reason, label in (
                (self.red_control_points, self.blue_control_points, 2, "累计控制度"),
                (self.red_total_damage, self.blue_total_damage, 3, "全队攻击伤害"),
                (red_remain, blue_remain, 4, "全队总剩余血量")):
            if red != blue:
                winner = 1 if red > blue else 2
                print(f"[server] 时间到，{'红方' if winner == 1 else '蓝方'}{label}占优，本局获胜")
                self._end_round(winner, reason)
                return
        self.round_winner = 0
        self.round_end_reason = 5
        print("[server] 时间到，双方控制度、伤害与剩余血量相同，本局平局")

    def _end_round_by_base_hp(self) -> None:
        # 规则 5.8.1：基地被击毁时比较双方基地剩余血量,高者获胜;相同则平局。
        if self.red_base_hp == self.blue_base_hp:
            winner = 0
        else:
            winner = 1 if self.red_base_hp > self.blue_base_hp else 2
        print(f"[server] 基地被击毁，{('平局', '红方获胜', '蓝方获胜')[winner]}")
        self._end_round(winner, 1)

    def _end_round(self, winner: int, reason: int) -> None:
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

        if self.radar_lock_override:
            self.target_locked = True
            self.target_id = self.radar_lock_override
        elif self.vision_online and self.autoaim:
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
            self._fortress_last = self.elapsed
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
            self._gain_economy(_FACTION_RED, amount)
            self._gain_economy(_FACTION_BLUE, amount)
            self._econ_paid += 1
        # 世界推进必须放在自机阵亡的提前返回之前：基地、前哨站、堡垒和控制度
        # 都与自机是否存活无关，放在后面会让自机一死整场比赛悄悄冻结。
        self.tick_field_hp()
        self.tick_fortress()
        self.tick_zones()
        self.track_gaps()
        self.tick_respawn()
        self.track_combat()
        self.tick_remote()
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
                self._register_kill(SELF_ROBOT_ID, self._random_killer(SELF_ROBOT_ID))
        if self.hp > 0 and self.rng.random() < 0.03:
            self.hp = min(self.max_hp, self.hp + 60)

    def _expire_temp_control(self, index: int) -> None:
        """临时控制的 5s 激活进度归零：由另一方转为完全控制（表 5-21）。"""
        before = self.zones[index]
        if index >= 2:
            self.zones[index] = _ZONE_RED_FULL if index == 2 else _ZONE_BLUE_FULL
            self._zone_changed(index, before)
            return
        holder = _FACTION_RED if self.zones[index] == _ZONE_RED_TEMP else _FACTION_BLUE
        challenger = _opponent(holder)
        self.zones[index] = _zone_full(challenger)
        if self._outpost_hp(index) == 0:
            self._rebuild_outpost(index, challenger)
        self._zone_changed(index, before)

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
        """按比赛秒推进堡垒激活计时，满 10 秒那一方的基地护甲展开。"""
        now = self.elapsed
        delta = min(max(now - self._fortress_last, 0.0), 1.0)
        self._fortress_last = now
        if self.fortress_auto and self.rng.random() < 0.01:
            self._set_fortress_attack(self.rng.choice((0, 1, 2)))
        side = self.fortress_attack
        if side == 0:
            return
        before = self.fortress_activation[side]
        self.fortress_activation[side] = min(10.0, before + delta)
        if before < 10.0 <= self.fortress_activation[side]:
            self._open_armor(side)

    def _deal_damage(self, victim_is_red: bool, amount: int) -> None:
        if victim_is_red:
            self.blue_total_damage += amount
            self.blue_hits += 1
        else:
            self.red_total_damage += amount
            self.red_hits += 1

    def _register_kill(self, victim_id: int, killer_id: int) -> None:
        """Event 1:参数「被击杀者,击杀者」,例如 "1,101" 是蓝方重装击毁红方重装。"""
        if _is_red(victim_id):
            self.blue_kills += 1
        else:
            self.red_kills += 1
        self.events.append((1, f"{victim_id},{killer_id}"))

    def _random_killer(self, victim_id: int) -> int:
        """随机掉血致死时编一个击杀者：对方场上的一台车。"""
        return self.rng.choice(_BLUE_FIELD_IDS if _is_red(victim_id) else _RED_FIELD_IDS)

    def _gain_economy(self, side: int, amount: int) -> None:
        if side == _FACTION_RED:
            self.red_economy += amount
            self.red_economy_total += amount
        else:
            self.blue_economy += amount
            self.blue_economy_total += amount

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
                self._deal_damage(_is_red(robot_id), applied)
                if self.other_hp[robot_id] == 0:
                    self._sync_respawn(robot_id)
                    self._register_kill(robot_id, self._random_killer(robot_id))
            elif self.rng.random() < 0.03:
                self.other_hp[robot_id] = min(
                    self.other_max_hp[robot_id], hp + 60)

        # 按 10Hz 折算:0.02 × 均值 50 × 10 tick ≈ 10 HP/s,1500 血约 150s 倒,比基地略早。
        # `> 0` 是边沿判定,不可省:血量已是 0 时再减仍是 0,只看减后结果会让前哨站倒下后每 tick 重播一次。
        for index, chance in ((1, 0.02), (0, 0.018)):
            hp = self._outpost_hp(index)
            if self.rng.random() < chance and hp > 0:
                applied = min(hp, self.rng.randint(20, 80))
                self._set_outpost_hp(index, hp - applied)
                self._deal_damage(self.outpost_owner[index] == _FACTION_RED, applied)
                if applied == hp:
                    self._destroy_outpost(index)

        if self.rng.random() < 0.02:
            applied = (0 if self._base_defended(_FACTION_BLUE)
                       else min(self.blue_base_hp, self.rng.randint(20, 90)))
            if applied:
                self.blue_base_hp -= applied
                self._deal_damage(False, applied)
                self._note_base_damage(_FACTION_BLUE, applied)
        if self.rng.random() < 0.018:
            applied = (0 if self._base_defended(_FACTION_RED)
                       else min(self.red_base_hp, self.rng.randint(20, 90)))
            if applied:
                self.red_base_hp -= applied
                self._deal_damage(True, applied)
                self._note_base_damage(_FACTION_RED, applied)

        # 收入期望需盖过支出期望,否则剩余经济几十秒内就被抽干贴在 0:
        # 收 0.18×27.5≈4.95/tick vs 支 0.05×80≈4.0/tick,净微增并在数百区间震荡。
        if self.rng.random() < 0.18:
            self._gain_economy(_FACTION_RED, self.rng.randint(10, 45))
        if self.rng.random() < 0.18:
            self._gain_economy(_FACTION_BLUE, self.rng.randint(10, 45))
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
        self.outpost_owner = [_FACTION_RED, _FACTION_BLUE]
        self.max_base_gap = 0
        self.max_control_gap = 0
        if _SELF_LOCAL_ID == 1:
            # 每局开局重装都没装 42mm 发射机构(表 5-16 开局默认 400 血)。
            self.launcher_ready = False
            self.launcher_verified = False
            self.max_hp = _ROBOT_MAX_HP[1]
        self.red_economy = _INITIAL_ECONOMY
        self.blue_economy = _INITIAL_ECONOMY
        self.red_economy_total = _INITIAL_ECONOMY
        self.blue_economy_total = _INITIAL_ECONOMY
        self._econ_paid = 0
        self.red_total_damage = 0
        self.blue_total_damage = 0
        self.fortress_attack = 0
        self.fortress_activation = {_FACTION_RED: 0.0, _FACTION_BLUE: 0.0}
        self._fortress_last = self.elapsed
        self.armor_open = {_FACTION_RED: False, _FACTION_BLUE: False}
        self.red_kills = 0
        self.blue_kills = 0
        self.red_energy_activations = 0
        self.blue_energy_activations = 0
        self.red_hits = 0
        self.blue_hits = 0
        self.dart_fixed_hits = {_FACTION_RED: 0, _FACTION_BLUE: 0}
        self.dart_base_hits = {_FACTION_RED: 0, _FACTION_BLUE: 0}
        self._auto_blind_fired = 0
        self.dart = {side: _new_dart_state() for side in (_FACTION_RED, _FACTION_BLUE)}
        self.tech_level = {_FACTION_RED: 0, _FACTION_BLUE: 0}
        self.assembly = None
        self.rune_status = 1
        self.rune_arms = 0
        self.rune_until = 0.0
        self.rune_big = False
        self.rune_used = {"small": 0, "big": 0}
        self.terrain = {}
        self.terrain_kinds = {}
        self.terrain_pre_until = 0.0
        self.remote_pending = []
        self._combat_at = None
        self.last_periodic_mark = -1e9
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
        self.fired_total = 0
        self.injury = _new_injury()
        self._pending_dart = 0
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
        """场上有血量的 8 台（自机是空中时不在其中）：(编号, 阵营, 血量, 上限)。"""
        out: list[tuple[int, int, int, int]] = []
        if SELF_ROBOT_ID in _RED_FIELD_IDS + _BLUE_FIELD_IDS:
            out.append((SELF_ROBOT_ID, _SELF_FACTION, self.hp, self.max_hp))
        for robot_id, hp in self.other_hp.items():
            out.append((robot_id, _FACTION_RED if _is_red(robot_id) else _FACTION_BLUE,
                        hp, self.other_max_hp[robot_id]))
        return sorted(out)

    def match_elapsed(self) -> float | None:
        stage, _ = self.stage()
        if stage != 4:
            return None
        return max(0.0, self.elapsed - self.round_start_e)

    def tick_blind(self) -> None:
        """推进模拟器自己的遮挡账本(给日志和面板),并按时刻表注入敌方飞镖命中。"""
        me = self.match_elapsed()
        if me is not None and _BLIND_AUTO:
            due = sum(at <= me for at, _ in _BLIND_SCHEDULE)
            while self._auto_blind_fired < due:
                at, target = _BLIND_SCHEDULE[self._auto_blind_fired]
                self._auto_blind_fired += 1
                # 跳转越过的命中不补发:几十秒前的命中现在才报,终端会在错的时刻切图传。
                if me - at <= 2.0:
                    self.trigger_blind(_SELF_FACTION, target)
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

        if active != self._blind_window:
            if active is not None:
                start, duration, side, target_type = active
                self._blind_window = active
                self.blind_side = side
                self.blind_target_type = target_type
                faction = "红方" if side == _FACTION_RED else "蓝方"
                target = _DART_TARGET_NAMES[target_type - 1]
                print(f"[server] 致盲开始 {faction} 目标={target} "
                      f"match_t={start:.1f}s 持续={duration:.1f}s", flush=True)
            else:
                self._blind_window = None
                self.blind_side = 0
                self.blind_target_type = 0
                self.blind_remaining_ms = 0
                print("[server] 致盲结束", flush=True)

        if self._blind_window is not None and me is not None:
            start, duration, _side, _target = self._blind_window
            self.blind_remaining_ms = max(0, int((start + duration - me) * 1000))

    def fabricated_other_poses(self) -> list[tuple[int, int, float, float, float]]:
        """编造的其他 7 台场上机器人位姿，终端经 RadarInfoToClient 收到（指挥手还经
        RobotPosition 收到己方哨兵）。

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
    client.subscribe([(T_CUSTOM_CONTROL, 0), (T_COMMON_COMMAND, 0), (T_SIM_LINK_CAR, 0),
                       (T_SIM_ARM_FOLLOW, 0), (T_ASSEMBLY_COMMAND, 0),
                       (T_PERFORMANCE_COMMAND, 0), (T_RUNE_COMMAND, 0),
                       (T_SENTRY_COMMAND, 0), (T_DART_COMMAND, 0), (T_MAP_COMMAND, 0),
                       (T_KEYBOARD_MOUSE, 0)])


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
    elif message.topic == T_SIM_ARM_FOLLOW:
        userdata.arm_follow = message.payload == b"1"
        print(f"[server] 机械臂回传标志 {'2（让控制器跟随车）' if userdata.arm_follow else '1（正常）'}")
    elif message.topic == T_SIM_LINK_CAR:
        online = message.payload != b"off"
        if online != userdata.car_online:
            print(f"[server] 车端数据 {'恢复' if online else '断电（CustomByteBlock 停发）'}")
        userdata.car_online = online
    elif message.topic == T_COMMON_COMMAND:
        m = pb.CommonCommand()
        m.ParseFromString(message.payload)
        name = _CTRL_NAMES.get(m.cmd_type)
        if name:
            print(f"[server] 收到赛程控制 <- {name} (id=0x{m.cmd_type:02X})")
            userdata.apply_control(m.cmd_type, m.param)
        else:
            userdata.handle_common(m.cmd_type, m.param)
    elif message.topic == T_ASSEMBLY_COMMAND and _SELF_LOCAL_ID == 1:
        m = pb.AssemblyCommand(); m.ParseFromString(message.payload)
        userdata.handle_assembly(m.operation, m.difficulty)
    elif message.topic == T_PERFORMANCE_COMMAND:
        m = pb.RobotPerformanceSelectionCommand(); m.ParseFromString(message.payload)
        userdata.handle_performance(m)
    elif message.topic == T_RUNE_COMMAND:
        m = pb.RuneActivateCommand(); m.ParseFromString(message.payload)
        if m.activate:
            userdata.activate_rune()
    elif message.topic == T_SENTRY_COMMAND and _SELF_LOCAL_ID == 6:
        m = pb.SentryCtrlCommand(); m.ParseFromString(message.payload)
        userdata.handle_sentry(client, m.command_id)
    elif message.topic == T_DART_COMMAND and _SELF_LOCAL_ID == 6:
        m = pb.DartCommand(); m.ParseFromString(message.payload)
        userdata.handle_dart_command(m)
    elif message.topic == T_MAP_COMMAND and _SELF_LOCAL_ID == 6:
        m = pb.MapClickCmd(); m.ParseFromString(message.payload)
        userdata.handle_map_click(client, m)
    elif message.topic == T_KEYBOARD_MOUSE:
        m = pb.KeyboardMouseControl()
        try:
            m.ParseFromString(message.payload)
        except Exception:
            return
        userdata.note_km(m)


def main() -> int:
    match = SimulatedMatch()
    match.car_online = True
    match.arm_joints = None
    match.arm_at = None
    match.arm_frames = 0
    match.arm_follow = False

    def handle_assembly(operation: int, difficulty: int) -> None:
        """重装装配(2.2.21):0 开始兑换 1 确认装配 2 取消装配。

        开始要站在装配区上、活着、不虚弱,难度已开放且装成了上一级。科技核心先运动 3 秒到位,
        之后每 3 秒做完一步(这一级要的步骤见 assembly_steps),做完才能确认。结果走 Event 15
        (0 成功 2 超时 3 离开装配区过久 4 重装战亡 6 主动退出)。"""
        me = match.match_elapsed()
        if me is None:
            print("[server] 装配指令被忽略:比赛不在进行", file=sys.stderr)
            return
        if operation == 0:
            limit = max_assembly_difficulty(me)
            done = match.tech_level[_SELF_FACTION]
            respawn = match.respawn_state.get(SELF_ROBOT_ID)
            if not 1 <= difficulty <= limit:
                print(f"[server] 装配难度 {difficulty} 还没解锁(当前最高 {limit})", file=sys.stderr)
                return
            if difficulty > done + 1 or (difficulty == 4 and done >= 4):
                print(f"[server] 装配难度 {difficulty} 不能选(已装成 {done} 级,要先装成上一级,四级只能装成一次)",
                      file=sys.stderr)
                return
            if not match.in_assembly_zone:
                print("[server] 重装不在装配区,不能开始装配", file=sys.stderr)
                return
            if (respawn is not None and respawn["dead"]) or match.is_weak(SELF_ROBOT_ID):
                print("[server] 重装阵亡或虚弱,不能开始装配", file=sys.stderr)
                return
            if match.assembly is None:
                match.assembly = {"difficulty": difficulty, "started": me}
                print(f"[server] 重装开始兑换,难度 {difficulty}")
        elif operation == 2 and match.assembly is not None:
            match.events.append((15, "6"))
            match.assembly = None
            print("[server] 重装取消装配")
        elif operation == 1 and match.assembly is not None:
            if not assembly_ready(int(match.assembly["difficulty"]), me - float(match.assembly["started"])):
                print("[server] 确认装配被忽略:步骤还没做完", file=sys.stderr)
                return
            done = int(match.assembly["difficulty"])
            match.tech_level[_SELF_FACTION] = max(match.tech_level[_SELF_FACTION], done)
            match.events.append((15, "0"))
            match.assembly = None
            print(f"[server] 重装装配成功,科技等级 {match.tech_level[_SELF_FACTION]}")

    def handle_sentry(client: mqtt.Client, command_id: int) -> None:
        """哨兵指令(2.2.31)。结果码:0 成功 1 姿态切换冷却中 2 金币不足 3 无效指令。"""
        now = match.match_elapsed() or 0.0
        cost = sentry_command_cost(command_id, now, match.sentry_auto)
        if command_id not in (1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
            result = 3
        elif command_id >= 7 and now - match.sentry_last_switch < 5:
            result = 1
        elif match.coin < cost:
            result = 2
        else:
            result = 0
            match._spend_coins(cost)
            if command_id >= 7:
                match.sentry_posture = ((command_id - 7) % 3) + 1
                match.sentry_powered = command_id >= 10
                match.sentry_last_switch = now
        print(f"[server] 哨兵指令 {command_id} -> 结果 {result}")
        _publish(client, T_SENTRY_RESULT, pb.SentryCtrlResult(command_id=command_id, result_code=result))

    def handle_dart_command(command) -> None:
        """飞镖(2.2.29):选目标、开闸、确认发射都走模拟器的飞镖规则(开闸窗口、冷却)。"""
        if 1 <= command.target_id <= len(_DART_TARGET_NAMES):
            match.dart_target = command.target_id
        error = None
        if command.launch_confirm:
            error = match.dart_shoot(_SELF_FACTION, match.dart_target)
        elif command.open and dart_gate_state() == 0:
            error = match.dart_open(_SELF_FACTION)
        if error:
            print(f"[server] 飞镖指令被拒: {error}", file=sys.stderr)

    def handle_map_click(client: mqtt.Client, command) -> None:
        now = time.monotonic()
        ascii_ok = command.mode != 4 or (0 < command.ascii < 128
                                         and chr(command.ascii) in "CDEFGHIJKLNOQRSTUVWXYZ")
        if now - match.last_map_click < .5 or not ascii_ok:
            print("[server] 地图标记被拒:间隔不足 0.5 秒或字母不允许", file=sys.stderr)
            return
        sentry_id = 7 if _SELF_FACTION == _FACTION_RED else 107
        to_sentry = command.is_send_all == 2 or (command.is_send_all == 0 and sentry_id in command.robot_id)
        fee = 50 if to_sentry and match.sentry_auto else 0
        if match.coin < fee:
            print("[server] 地图标记被拒:转发给自动哨兵要 50 金币,余额不足", file=sys.stderr)
            return
        match._spend_coins(fee)
        match.last_map_click = now
        print(f"[server] 收到地图标记 类型={command.mode} 模式={command.type} "
              f"({command.map_x:.1f}, {command.map_y:.1f})")
        _publish(client, T_MAP_INFO, pb.MapClickInfo(is_send_all=command.is_send_all,
            robot_id=command.robot_id, mode=command.mode, enemy_id=command.enemy_id,
            ascii=command.ascii, type=command.type, map_x=command.map_x, map_y=command.map_y))

    def dart_gate_state() -> int:
        """DartSelectTargetStatusSync.open:0 关闭 1 开启中 2 已开启。"""
        me = match.match_elapsed()
        state = match.dart[_SELF_FACTION]
        if me is None or state["open_at"] < 0 or me > state["close_at"]:
            return 0
        return 1 if me < state["open_at"] + _DART_GATE_OPEN_SEC else 2

    def activate_rune() -> None:
        """5.6.3:只有步兵操作手(和哨兵)能开;要有激活机会,而且能量机关处于未激活。"""
        me = match.match_elapsed()
        reason = None
        if _SELF_LOCAL_ID not in (3, 4):
            reason = "只有步兵能开能量机关"
        elif me is None:
            reason = "比赛未开始或已结束"
        elif match.rune_status != 1:
            reason = "能量机关不是未激活状态"
        elif match.rune_chances() <= 0:
            reason = "现在没有激活机会"
        if reason:
            print(f"[server] 开能量机关失败: {reason}", file=sys.stderr)
            return
        match.rune_used["small" if me < 180.0 else "big"] += 1
        match.rune_status = 2
        match.rune_arms = 0
        match.rune_until = me + _RUNE_ACTIVATING_SEC
        print("[server] 我方开始激活能量机关")

    def send_buff() -> None:
        """2.2.14:自机身上每一种增益 1Hz 发一条,失去后不再发(站在兑换点上是类型 6)。"""
        for buff_type, level, longest, left in match.self_buffs():
            _publish(client, T_BUFF, pb.Buff(robot_id=SELF_ROBOT_ID, buff_type=buff_type, buff_level=level,
                                              buff_max_time=longest, buff_left_time=left))

    def send_mark(client: mqtt.Client, kind: int) -> None:
        """模拟指挥手发一个标记:标在敌方重装身上,所有己方车(含哨兵)都收得到。"""
        enemy_hero = 101 if _SELF_FACTION == _FACTION_RED else 1
        poses = {rid: (x, y) for rid, _f, x, y, _yaw in match.fabricated_other_poses()}
        x, y = poses.get(enemy_hero, (_FIELD_LENGTH_M / 2, _FIELD_WIDTH_M / 2))
        _publish(client, T_MAP_INFO, pb.MapClickInfo(
            is_send_all=2, robot_id=bytes(7), mode=kind, enemy_id=enemy_hero, ascii=0,
            type=2, map_x=x, map_y=y))
        print(f"[server] 指挥手标记 类型={kind} -> 敌方重装 ({x:.1f}, {y:.1f})")

    match.handle_assembly = handle_assembly
    match.handle_sentry = handle_sentry
    match.handle_dart_command = handle_dart_command
    match.handle_map_click = handle_map_click
    match.activate_rune = activate_rune

    def handle_common(cmd_type: int, param: int) -> None:
        """终端的 CommonCommand(2.2.25)。成功打一行,失败把原因打到 stderr。"""
        actions = {
            1: (f"兑换 17mm x{param}", lambda: match.buy_ammo(17, param)),
            2: (f"兑换 42mm x{param}", lambda: match.buy_ammo(42, param)),
            3: ("确认复活", match.confirm_revive),
            4: ("兑换立即复活", match.pay_revive),
            5: (f"远程兑换 {match.caliber}mm x{param}", lambda: match.remote_buy_ammo(param)),
            6: ("远程兑换血量", match.remote_heal),
        }
        if cmd_type not in actions:
            print(f"[server] 未知 CommonCommand cmd_type={cmd_type} param={param}", file=sys.stderr)
            return
        what, act = actions[cmd_type]
        error = act()
        if error is None:
            print(f"[server] 终端指令 {what} 成功,队伍余额 {match.coin}", flush=True)
        else:
            print(f"[server] 终端指令 {what} 失败: {error}", file=sys.stderr, flush=True)

    def handle_performance(command) -> None:
        """2.2.23:步兵选性能体系;指挥手给哨兵选控制方式(准备阶段、自检之前)。"""
        if command.HasField("sentry_control"):
            if _SELF_LOCAL_ID == 6 and match.stage()[0] == _STAGE_PREP:
                match.sentry_auto = command.sentry_control == 0
                print(f"[server] 哨兵控制方式 -> {'自动' if match.sentry_auto else '半自动'}")
            else:
                print("[server] 哨兵控制方式只能由指挥手在准备阶段选", file=sys.stderr)
        if not (command.HasField("shooter") or command.HasField("chassis")):
            return
        error = match.select_performance(command.shooter if command.HasField("shooter") else None,
                                         command.chassis if command.HasField("chassis") else None)
        if error is None:
            print(f"[server] 性能体系 -> 底盘 {match.chassis_mode} 发射 {match.shooter_mode}"
                  "(1 血量/冷却优先 2 功率/爆发优先)", flush=True)
        else:
            print(f"[server] 选性能体系失败: {error}", file=sys.stderr, flush=True)

    match.handle_common = handle_common
    match.handle_performance = handle_performance
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
                print(f"[server] 重装装上 42mm 发射机构,还差验证弹,Q0={match.heat_limit}")
            else:
                match.say(f"装42mm 没成功:{error}")
                print(f"[server] 安装失败: {error}", file=sys.stderr)
        elif cmd == CTRL_VERIFY_LAUNCHER:
            error = match.verify_launcher()
            if error is None:
                print(f"[server] 验证弹被测到,重装成功安装 42mm 发射机构,HP={match.hp}/{match.max_hp}")
            else:
                match.say(f"打验证弹没成功:{error}")
                print(f"[server] 验证弹失败: {error}", file=sys.stderr)
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
            said = match.note_seq
            if match.set_zone(zone, state):
                print(f"[server] {_ZONE_NAMES[zone]} -> {_ZONE_STATE_NAMES[state]}")
            else:
                if match.note_seq == said:
                    match.say(f"阵地没改:序号 {zone} 不能设成状态 {state}")
                print(f"[server] 阵地没改 序号={zone} 状态={state}: {match.note}", file=sys.stderr)
        elif cmd == CTRL_SET_STRUCTURE_HP:
            index, hp = divmod(param, 10000)
            said = match.note_seq
            if match.set_structure_hp(index, hp):
                print(f"[server] {_STRUCTURE_NAMES[index]} 血量设为 "
                      f"{min(hp, _STRUCTURE_MAX_HP[index])}")
            else:
                if match.note_seq == said:
                    match.say(f"建筑序号 {index} 不对")
                print(f"[server] 建筑血量没改 序号={index}: {match.note}", file=sys.stderr)
        elif cmd == CTRL_SET_FORTRESS:
            names = {0: "停止激活", 1: "蓝方在激活红方堡垒", 2: "红方在激活蓝方堡垒", 3: "随机"}
            if match.set_fortress(param):
                print(f"[server] 堡垒 -> {names[param]}")
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
            error = match.trigger_blind(side, target_type)
            if error is None:
                faction = "红方" if side == _FACTION_RED else "蓝方"
                print(f"[server] {faction}中镖 "
                      f"目标={_DART_TARGET_NAMES[target_type - 1]}")
            else:
                match.say(f"中镖没成功:{error}")
                print(f"[server] 中镖失败: {error}", file=sys.stderr)
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
                match.say(f"开闸没成功:{error}")
                print(f"[server] 开闸失败: {error}", file=sys.stderr)
        elif cmd == CTRL_DART_SHOOT:
            side, target_type = divmod(param, 10)
            error = match.dart_shoot(side, target_type)
            if error is None:
                print(f"[server] {'红方' if side == _FACTION_RED else '蓝方'}飞镖已发射")
            else:
                match.say(f"发射没成功:{error}")
                print(f"[server] 发射失败: {error}", file=sys.stderr)
        elif cmd == CTRL_MARK:
            if param in (1, 2, 3):
                send_mark(client, param)
            else:
                print(f"[server] 非法标记类型 {param}", file=sys.stderr)
        elif cmd == CTRL_ASSEMBLY_ZONE:
            match.in_assembly_zone = param != 0
            print(f"[server] 重装{'开进' if match.in_assembly_zone else '开出'}装配区")
            # 规则手册 5.6.4.7:装配中离开装配区过久算失败。模拟器开出去就算。
            if not match.in_assembly_zone and match.assembly is not None:
                match.events.append((15, "3"))
                match.assembly = None
        elif cmd == CTRL_EXCHANGE_POINT:
            match.at_exchange_point = param != 0
            print(f"[server] 车{'开进' if match.at_exchange_point else '开出'}兑换点")
            # 获得增益时当场发一次(2.2.14),不等下一个 1 秒。
            send_buff()
        elif cmd == CTRL_ENEMY_RUNE:
            error = match.enemy_rune(param == 2)
            if error is None:
                print(f"[server] 敌方{'大' if param == 2 else '小'}能量机关已激活(只发事件 3/4)")
            else:
                print(f"[server] 敌方开能量机关失败: {error}", file=sys.stderr)
        elif cmd == CTRL_RADAR_LOCK:
            if param == 0:
                match.radar_lock_override = 0
                print("[server] 解除强制雷达标记")
            elif param in _ENEMY_IDS:
                match.radar_lock_override = param
                print(f"[server] 雷达强制标记 {_robot_name(param)}")
            else:
                print(f"[server] 雷达标记的车号不对: {param}", file=sys.stderr)
        elif cmd == CTRL_TERRAIN:
            error = match.gain_terrain(param)
            if error is None:
                name = "高地差一步" if param == _TERRAIN_PRE_HIGHLAND else _TERRAIN[param][0]
                print(f"[server] 自机拿到地形增益:{name}")
                send_buff()
            else:
                print(f"[server] 地形增益没拿到: {error}", file=sys.stderr)

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

    def send_arm_feedback() -> None:
        # 2027：车经 0x0310 把机械臂姿态回传给终端，终端转给控制器。跟随模式下车报一个自己
        # 在动的姿态，控制器收到标志 2 就会跟着车走；平时报它正在跟随的那个姿态。
        if not match.car_online:
            return
        if match.arm_follow:
            joints = [30 * math.sin(time.monotonic() / 2), 30, -20, 0, 10, 45]
            flag = FOLLOW_FLAG
        else:
            joints = match.arm_joints or [0.0] * 6
            flag = 1
        _publish(client, T_CUSTOM_BYTE_BLOCK, pb.CustomByteBlock(data=feedback_payload(joints, flag)))

    def send_game_status() -> None:
        stage, remain = match.stage()
        _publish(client, T_GAME_STATUS, pb.GameStatus(
            current_round=match.round_index + 1, total_rounds=3,
            red_score=match.red_score, blue_score=match.blue_score,
            current_stage=stage, stage_countdown_sec=remain,
            stage_elapsed_sec=int(match.match_elapsed() or 0),
            is_paused=match.holding and stage == _STAGE_MATCH,
            game_result=match.winner(), end_reason=match.round_end_reason if stage == 5 else 255,
        ))
        client.publish(T_SIM_AUTO_HP, b"1" if match.auto_hp else b"0", retain=True)

    def send_dynamic() -> None:
        match.tick_10hz()
        _publish(client, T_ROBOT_DYNAMIC, pb.RobotDynamicStatus(
            current_health=match.hp, current_heat=match.heat,
            last_projectile_fire_rate=29.4 + .4 * math.sin(match.elapsed),
            current_buffer_energy=int(max(0, 60 - 25 * abs(math.sin(match.elapsed * 1.1)))),
            current_experience=match.level * 100, experience_for_upgrade=100,
            total_projectiles_fired=match.fired_total, remaining_ammo=match.ammo,
            is_out_of_combat=match.out_of_combat(),
            out_of_combat_countdown=match.out_of_combat_countdown(),
            can_remote_heal=match._remote_check() is None,
            can_remote_ammo=match._remote_check() is None and match.launcher_ready,
        ))

    def send_injury() -> None:
        hurt = match.injury
        _publish(client, T_INJURY, pb.RobotInjuryStat(
            total_damage=sum(hurt.values()), collision_damage=hurt["collision"],
            small_projectile_damage=hurt["small"], large_projectile_damage=hurt["large"],
            dart_splash_damage=hurt["dart"], module_offline_damage=hurt["module"],
            offline_damage=hurt["offline"], penalty_damage=hurt["penalty"],
            server_kill_damage=hurt["server"], killer_id=0))

    def send_module() -> None:
        rng = match.rng
        small, big = match.shooter_modules()
        _publish(client, T_ROBOT_MODULE, pb.RobotModuleStatus(
            power_manager=1, main_controller=1, armor=1,
            small_shooter=small, big_shooter=big,
            video_transmission=1,
            capacitor=1 if rng.random() > 0.1 else 0,
            uwb=1, rfid=1, light_strip=1,
            modular_42mm=1 if _SELF_LOCAL_ID == 1 and match.launcher_permission() else 0,
        ))

    def send_position() -> None:
        x, y, yaw = _self_pose(_pose_tick(match.elapsed))
        _publish(client, T_ROBOT_POSITION, pb.RobotPosition(x=x, y=y, z=0, yaw=yaw, robot_id=SELF_ROBOT_ID))
        # 2.2.13:指挥手还收到己方哨兵的坐标(robot_id 7/107)。
        if _SELF_LOCAL_ID == 6:
            sentry_id = 7 if _SELF_FACTION == _FACTION_RED else 107
            for rid, _f, sx, sy, syaw in match.fabricated_other_poses():
                if rid == sentry_id:
                    _publish(client, T_ROBOT_POSITION,
                             pb.RobotPosition(x=sx, y=sy, z=0, yaw=syaw, robot_id=sentry_id))

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

    def send_events() -> None:
        while match.events:
            event_id, param = match.events.pop(0)
            _publish(client, T_EVENT, pb.Event(event_id=event_id, param=param))

    def hp_for(robot_id: int) -> int:
        if robot_id == SELF_ROBOT_ID:
            return match.hp
        return match.other_hp[robot_id]

    def bullets_for(robot_id: int) -> int:
        if robot_id == SELF_ROBOT_ID:
            return match.ammo
        return match.other_ammo.get(robot_id, 0)

    def send_global_unit() -> None:
        """2.2.4:己方/对方相对自机;R/B 前哨站血量是绝对的,R/B 前哨阵地状态是相对的。"""
        own_red = _SELF_FACTION == _FACTION_RED
        own_base, enemy_base = ((match.red_base_hp, match.blue_base_hp) if own_red
                                else (match.blue_base_hp, match.red_base_hp))
        own_zone, enemy_zone = ((match.zones[2], match.zones[3]) if own_red
                                else (match.zones[3], match.zones[2]))
        own_cp, enemy_cp = ((match.red_control_points, match.blue_control_points) if own_red
                            else (match.blue_control_points, match.red_control_points))
        own_damage, enemy_damage = ((match.red_total_damage, match.blue_total_damage) if own_red
                                    else (match.blue_total_damage, match.red_total_damage))
        ids = ordered_robot_ids(_SELF_FACTION)
        _publish(client, T_GLOBAL_UNIT, pb.GlobalUnitStatus(
            base_health=own_base,
            base_status=base_status_code(match._base_defended(_SELF_FACTION),
                                         match.armor_open[_SELF_FACTION]),
            base_shield=relative_base_shield(own_zone, _SELF_FACTION, False),
            outpost_R_health=match.red_outpost_hp,
            outpost_R_status=relative_outpost_status(match.zones[0], _SELF_FACTION),
            enemy_base_health=enemy_base,
            enemy_base_status=base_status_code(match._base_defended(_ENEMY_FACTION),
                                               match.armor_open[_ENEMY_FACTION]),
            enemy_base_shield=relative_base_shield(enemy_zone, _SELF_FACTION, True),
            outpost_B_health=match.blue_outpost_hp,
            outpost_B_status=relative_outpost_status(match.zones[1], _SELF_FACTION),
            robot_health=[hp_for(robot_id) for robot_id in ids],
            robot_bullets=[bullets_for(robot_id) for robot_id in ids[:4]],
            total_damage_ally=own_damage, total_damage_enemy=enemy_damage,
            total_CP_ally=own_cp, total_CP_enemy=enemy_cp))

    def send_logistics() -> None:
        own_red = _SELF_FACTION == _FACTION_RED
        _publish(client, T_GLOBAL_LOGISTICS, pb.GlobalLogisticsStatus(
            remaining_economy=match.red_economy if own_red else match.blue_economy,
            total_economy_obtained=match.red_economy_total if own_red else match.blue_economy_total,
            tech_level=match.tech_level[_SELF_FACTION], encryption_level=0))

    def send_static() -> None:
        _publish(client, T_ROBOT_STATIC, pb.RobotStaticStatus(connection_state=1, field_state=0,
            alive_state=1 if match.hp else 2, robot_id=SELF_ROBOT_ID, robot_type=_SELF_LOCAL_ID,
            performance_system_shooter=match.shooter_mode, performance_system_chassis=match.chassis_mode,
            level=match.level,
            max_health=match.max_hp, max_heat=match.heat_limit, heat_cooldown_rate=match.cooling_rate,
            max_power=80, max_buffer_energy=60, max_chassis_energy=200))

    def send_radar() -> None:
        """2.2.19:对方 1,3,4,7、己方 1,3,4,7 共 8 份,单位 cm;阵亡或没有位置报 0,0。
        自瞄锁定的那台标成特殊标识 1。"""
        poses = {robot_id: (x, y) for robot_id, _faction, x, y, _yaw in match.fabricated_other_poses()}
        poses[SELF_ROBOT_ID] = _self_pose(_pose_tick(match.elapsed))[:2]
        radar = pb.RadarInfoToClient()
        for robot_id in radar_robot_ids(_SELF_FACTION):
            entry = radar.robots.add()
            if robot_id in poses and hp_for(robot_id) > 0:
                entry.target_pos_x, entry.target_pos_y = map(radar_centimetres, poses[robot_id])
            entry.is_high_light = 1 if match.target_locked and robot_id == match.target_id else 0
        _publish(client, T_RADAR_INFO, radar)

    def send_special() -> None:
        """2.2.6:只列正在生效的机制。1 我方堡垒被激活 2 敌方堡垒被我方激活;3–6 阵地激活
        计时,>0 我方在推进。"""
        ids, seconds = [], []
        attacked = match.fortress_attack
        if attacked:
            ids.append(1 if attacked == _SELF_FACTION else 2)
            seconds.append(int(match.fortress_activation[attacked]))
        for index, progress in enumerate(match.zone_progress):
            if progress:
                ids.append(index + 3)
                pushing_red = match.zones[index] in (_ZONE_RED_TEMP, _ZONE_RED_FULL)
                ours = pushing_red == (_SELF_FACTION == _FACTION_RED)
                seconds.append(int(math.ceil(progress)) * (1 if ours else -1))
        _publish(client, T_GLOBAL_SPECIAL, pb.GlobalSpecialMechanism(mechanism_id=ids, mechanism_time_sec=seconds))

    def send_performance() -> None:
        """2.2.24 步兵性能体系同步。重装、空中、哨兵席位没有性能体系,不发。"""
        if _SELF_LOCAL_ID in (3, 4):
            _publish(client, T_PERFORMANCE_SYNC, pb.RobotPerformanceSelectionSync(
                shooter=match.shooter_mode, chassis=match.chassis_mode,
                sentry_control=0 if match.sentry_auto else 1))

    def send_rune() -> None:
        """2.2.27 我方能量机关:激活 5 秒后进入已激活,45 秒后回到未激活。"""
        me = match.match_elapsed()
        if me is not None and match.rune_status == 2:
            match.rune_arms = min(10, int((me - (match.rune_until - _RUNE_ACTIVATING_SEC)) * 2))
            if me >= match.rune_until:
                big = me >= 180
                match.rune_status = 3
                match.rune_big = big
                match.rune_until = me + (_BIG_RUNE_ACTIVE_SEC if big else _RUNE_ACTIVE_SEC)
                if big:
                    match.events.append((3, _BIG_RUNE_EVENT))
                match.events.append((4, "2" if big else "1"))
                print(f"[server] 我方{'大' if big else '小'}能量机关已激活")
        elif me is not None and match.rune_status == 3 and me >= match.rune_until:
            match.rune_status = 1
            match.rune_arms = 0
        _publish(client, T_RUNE_STATUS, pb.RuneStatusSync(
            rune_status=match.rune_status, activated_arms=match.rune_arms,
            average_rings=9.2 if match.rune_status == 2 else 0.0))

    def sentry_path(sentry_id: int) -> pb.RobotPathPlanInfo:
        """哨兵从当前位置走向场地中央的 49 段路径,起点和增量都是分米。"""
        poses = {rid: (x, y) for rid, _f, x, y, _yaw in match.fabricated_other_poses()}
        sx, sy = poses.get(sentry_id, (_FIELD_LENGTH_M / 2, _FIELD_WIDTH_M / 2))
        tx, ty = _FIELD_LENGTH_M / 2, _FIELD_WIDTH_M / 2 + 2.5 * math.sin(match.elapsed * 0.1)
        path = pb.RobotPathPlanInfo(intention=match.sentry_posture,
                                    start_pos_x=int(round(sx * 10)), start_pos_y=int(round(sy * 10)),
                                    sender_id=sentry_id)
        px, py = path.start_pos_x, path.start_pos_y
        for step in range(1, 50):
            nx = int(round((sx + (tx - sx) * step / 49) * 10))
            ny = int(round((sy + (ty - sy) * step / 49) * 10))
            path.offset_x.append(max(-128, min(127, nx - px)))
            path.offset_y.append(max(-128, min(127, ny - py)))
            px, py = px + path.offset_x[-1], py + path.offset_y[-1]
        return path

    def send_role_status() -> None:
        now = match.match_elapsed()
        match.tick_blind()
        if _SELF_LOCAL_ID == 1:
            assembly = match.assembly
            elapsed = 0.0 if assembly is None or now is None else now - float(assembly["started"])
            respawn = match.respawn_state.get(SELF_ROBOT_ID)
            if assembly is not None and respawn is not None and respawn["dead"]:
                match.events.append((15, "4"))
                match.assembly = None
                assembly = None
                print("[server] 重装战亡,装配失败")
            elif assembly is not None and assembly_timed_out(int(assembly["difficulty"]), elapsed):
                match.events.append((15, "2"))
                match.assembly = None
                assembly = None
                print("[server] 装配超时")
            _publish(client, T_TECH_CORE, pb.TechCoreMotionStateSync(
                maximum_difficulty_level=max_assembly_difficulty(now or 0.0), enemy_core_status=0,
                **tech_core_state(None if assembly is None else int(assembly["difficulty"]), elapsed)))
        if _SELF_LOCAL_ID == 6:
            sentry_id = 7 if _SELF_FACTION == _FACTION_RED else 107
            _publish(client, T_SENTRY_STATUS, pb.SentryStatusSync(
                posture_id=match.sentry_posture, is_weakened=match.is_weak(sentry_id),
                is_powered=match.sentry_powered))
            _publish(client, T_DART_STATUS, pb.DartSelectTargetStatusSync(
                target_id=match.dart_target, open=dart_gate_state()))
            if now is not None and int(match.elapsed) % 2 == 0:
                _publish(client, T_PATH_PLAN, sentry_path(sentry_id))
        elif now is not None and now - match.last_periodic_mark >= _MARK_PERIOD_SEC:
            # 别的席位:模拟指挥手隔一段时间标一次敌方重装,终端地图上能看到收到的标记。
            match.last_periodic_mark = now
            send_mark(client, 1)

    def send_sim_state() -> None:
        """仅模拟器:绝对红蓝视角的状态快照,给控制面板和验收脚本读。终端不订阅。"""
        stage, remain = match.stage()
        hp = {str(rid): [h, mx] for rid, _f, h, mx in match.field_health()}
        client.publish(T_SIM_STATE, json.dumps({
            "stage": stage, "remain": remain, "elapsed": int(match.match_elapsed() or 0),
            "seat": SELF_ROBOT_ID, "zones": list(match.zones),
            "control": [match.red_control_points, match.blue_control_points],
            "base": [match.red_base_hp, match.blue_base_hp],
            "outpost": [match.red_outpost_hp, match.blue_outpost_hp],
            "economy": [match.red_economy, match.blue_economy],
            "reinforce": [match.red_reinforce_chances, match.blue_reinforce_chances],
            "fortress": [match.fortress_attack, match.fortress_auto,
                         round(match.fortress_activation[match.fortress_attack], 1)
                         if match.fortress_attack else 0.0],
            "armor": [match.armor_open[_FACTION_RED], match.armor_open[_FACTION_BLUE]],
            "blind": [match.blind_side, match.blind_target_type, match.blind_remaining_ms],
            "hp": hp, "self_hp": [match.hp, match.max_hp],
            "tech_level": match.tech_level[_SELF_FACTION],
            "assembly": None if match.assembly is None else int(match.assembly["difficulty"]),
            "dart_gate": dart_gate_state(), "dart_target": match.dart_target,
            "sentry": [match.sentry_posture, match.sentry_powered],
            "rune": match.rune_status, "rune_chances": match.rune_chances(),
            "performance": [match.chassis_mode, match.shooter_mode],
            "combat": [match.out_of_combat(), match.out_of_combat_countdown()],
            "respawn": list(match.respawn_status()[:4]),
            "self_ammo": match.ammo,
            "outpost_owner": list(match.outpost_owner),
            "launcher": [match.launcher_ready, match.launcher_verified, match.launcher_permission(),
                         round(match.launcher_unlock_sec())],
            "note": match.note, "note_seq": match.note_seq,
            "exchange_point": match.at_exchange_point,
            "assembly_zone": match.in_assembly_zone,
            "km": match.km_summary(),
        }))

    workers = [
        # 50Hz 云台/自瞄/目标状态:雷达的「被标记」就来自这里的 target_locked(以前没人调,模拟器从不标记)。
        threading.Thread(target=loop, args=(1 / 50, match.tick_fast), daemon=True),
        threading.Thread(target=loop, args=(1 / 10, send_arm_feedback), daemon=True),
        threading.Thread(target=loop, args=(1 / 5, send_game_status), daemon=True),
        threading.Thread(target=loop, args=(1 / 10, send_dynamic), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_module), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_buff), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_injury), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_position), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_global_unit), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_logistics), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_special), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_static), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_radar), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_role_status), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_rune), daemon=True),
        threading.Thread(target=loop, args=(1.0, send_performance), daemon=True),
        threading.Thread(target=loop, args=(1 / 2, send_sim_state), daemon=True),
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
            km = match.km_summary()
            if km:
                print(f"[server] 终端键鼠 {km['hz']} 帧/秒 键[{km['keys']}] 鼠标键[{km['buttons']}] "
                      f"鼠标一秒位移 {km['mouse']}", flush=True)
    except KeyboardInterrupt:
        print("\n[server] 停止")
    finally:
        stop.set()
        client.loop_stop()
        client.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
