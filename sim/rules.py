"""2027 规则手册中的纯数据与公式。

这里只放不依赖比赛状态的查表、取整和系数计算，状态推进留在 SimulatedMatch。
所有时间参数都是「比赛秒」，调用方负责把挂钟换算成比赛秒；暂停和倍速因此不会
悄悄放大规则。

手册没写取整方式的地方（例如黄牌百分比）按四舍五入处理，并在函数上注明。
"""

from __future__ import annotations

import math

MATCH_SEC = 420.0

# 表 5-15 步兵等级与总经验，最高 10 级。
LEVEL_EXP = (0, 550, 1100, 1650, 2200, 2750, 3300, 3850, 4400, 5000)
MAX_LEVEL = len(LEVEL_EXP)

# 表 5-17 步兵底盘上限血量。
CHASSIS_HP_POWER = (150, 175, 200, 225, 250, 275, 300, 325, 350, 400)
CHASSIS_HP_BLOOD = (200, 225, 250, 275, 300, 325, 350, 375, 400, 400)

# 表 5-18 步兵 17mm 发射机构：(射击热量上限, 每秒冷却值)。
LAUNCHER_BURST = ((170, 5), (180, 7), (190, 9), (200, 11), (210, 12),
                  (220, 13), (230, 14), (240, 16), (250, 18), (260, 20))
LAUNCHER_COOLING = ((40, 12), (48, 14), (56, 16), (64, 18), (72, 20),
                    (80, 22), (88, 24), (96, 26), (104, 28), (120, 30))

# 表 5-16 重装（完成三级装配/装 42mm 模块）与表 5-17 默认选择。
HEAVY_MAX_HP = 600
HEAVY_HEAT_LIMIT = 300
HEAVY_COOLING = 100.0
# 自动哨兵由规则 5.1 给出；半自动哨兵为 100/10。
SENTRY_AUTO_HEAT = (260, 30.0)
SENTRY_SEMI_HEAT = (100, 10.0)

# 5.1.3 单发热量、Q2 增量与 10Hz 冷却。
PELLET_HEAT = {17: 10, 42: 100}
Q2_EXTRA = {17: 100, 42: 200}
COOLING_HZ = 10.0

# 5.3.1 回血与脱战。
SUPPLY_REGEN_FRACTION = 0.10
SUPPLY_REGEN_LATE_FRACTION = 0.25
SUPPLY_REGEN_LATE_AFTER = 240.0      # 比赛开始后满 4 分钟（elapsed）
OUT_OF_COMBAT_SEC = 6.0
REMOTE_HEAL_DELAY_SEC = 6.0
REMOTE_HEAL_FRACTION = 0.60

# 5.3.2 复活。
REVIVE_INVINCIBLE_SEC = 30.0
REVIVE_WEAK_INVINCIBLE_FLOOR_SEC = 10.0
REVIVE_HP_FRACTION = 0.10
PAY_REVIVE_WEAK_SEC = 3.0
PAY_REVIVE_HP_FRACTION = 1.0
REVIVE_RATE_NORMAL = 1.0
REVIVE_RATE_BOOSTED = 4.0
LOW_BASE_HP = 4000

# 5.4.1 表 5-7 兑换。
AMMO_CAP = {17: 1000, 42: 100}
AMMO_EXCHANGE = {  # (每份发数, 每份金币)
    (17, True): (100, 150), (17, False): (10, 10),
    (42, True): (10, 150), (42, False): (1, 10),
}
SENTRY_INITIAL_AMMO = 300
SENTRY_SUPPLY_ROUNDS = 100

# 7.1.2 表 7-2 黄牌。
CARD_OFFENDER_FRACTION = 0.15
CARD_TEAMMATE_FRACTION = 0.05
CARD_DOUBLE_WINDOW_SEC = 30.0
CARD_RED_AFTER = 3


def round_half_up(value: float) -> int:
    """手册的「四舍五入」；Python 的 round() 是银行家舍入，不能直接用。"""
    return int(math.floor(value + 0.5))


def clamp_hp(hp: float, maximum: int) -> int:
    return max(0, min(maximum, int(hp)))


def level_for_exp(exp: int) -> int:
    level = 1
    for index, need in enumerate(LEVEL_EXP, start=1):
        if exp >= need:
            level = index
    return level


def kill_exp(victim_level: int, killer_level: int) -> int:
    """表 5-14：被击毁者等级更高时才有等级差加成。"""
    if victim_level < killer_level:
        return 50 * victim_level
    return round_half_up(50 * victim_level * (1 + 0.2 * (victim_level - killer_level)))


def damage_exp(target: str, damage: int) -> int:
    """伤害经验：机器人 4/点，前哨站 2/点，基地每 2 点 1 点且奇数向上取整。"""
    if target == "robot":
        return damage * 4
    if target == "outpost":
        return damage * 2
    if target == "base":
        return math.ceil(damage / 2)
    raise ValueError(f"未知的经验目标 {target}")


def logistics_lambda(base_hp: int) -> float:
    """5.6.1：λ = 1 - 已损失基地血量 / 20000。"""
    return 1.0 - (10000 - max(0, min(10000, base_hp))) / 20000.0


def revive_delay(remaining: float, paid_count: int, lam: float) -> int:
    """复活读条 = (10 + (420-剩余)/10 + 20*兑换次数) / λ，取整到秒。"""
    base = 10 + (MATCH_SEC - remaining) / 10 + 20 * paid_count
    return max(1, round_half_up(base / lam))


def revive_rate(in_supply: bool, base_hp: int) -> float:
    """读条速率：补给区增益点或己方基地 < 4000 时 4/s，否则 1/s。"""
    return REVIVE_RATE_BOOSTED if in_supply or base_hp < LOW_BASE_HP else REVIVE_RATE_NORMAL


def pay_revive_cost(remaining: float, level: int) -> int:
    """表 5-7：ROUNDUP((420-剩余)/60)*80 + 等级*20。"""
    return math.ceil((MATCH_SEC - remaining) / 60) * 80 + level * 20


def supply_regen_rate(max_hp: int, elapsed: float, out_of_combat: bool,
                      lam: float) -> float:
    """补给区每秒回血；比赛满 4 分钟且脱战时为 25%，否则 10%，再乘 λ。"""
    fraction = (SUPPLY_REGEN_LATE_FRACTION
                if elapsed >= SUPPLY_REGEN_LATE_AFTER and out_of_combat
                else SUPPLY_REGEN_FRACTION)
    return max_hp * fraction * lam


def outpost_regen_rate(max_hp: int, elapsed: float, out_of_combat: bool,
                       lam: float) -> float:
    """前哨站回血 = 此刻补给区回血速率的 50%。"""
    return 0.5 * supply_regen_rate(max_hp, elapsed, out_of_combat, lam)


def remote_heal_cost(remaining: float) -> int:
    """表 5-7：50 + ROUNDUP((420-剩余)/60*20)。注意 20 在取整括号内。"""
    return 50 + math.ceil((MATCH_SEC - remaining) / 60 * 20)


def remote_heal_amount(max_hp: int, lam: float) -> int:
    return clamp_hp(max_hp * REMOTE_HEAL_FRACTION * lam, max_hp)


def ammo_exchange_cost(caliber: int, rounds: int, remote: bool) -> int | None:
    """返回金币价；发数不是最小兑换单位整数倍时返回 None。"""
    try:
        unit, price = AMMO_EXCHANGE[(caliber, remote)]
    except KeyError:
        return None
    if rounds <= 0 or rounds % unit:
        return None
    return (rounds // unit) * price


def q2_limit(caliber: int, q0: int) -> int:
    return q0 + Q2_EXTRA[caliber]


def heat_locked(heat: float, q0: int) -> bool:
    """Q1 > Q0 时发射机构锁定、图传断开，直到 Q1 冷却到 0。"""
    return heat > q0


def heat_permanently_locked(heat: float, q0: int, caliber: int) -> bool:
    """Q1 ≥ Q2 时当局后续全部锁定（流程图用 ≥，正文用 >，取更保守的 ≥）。"""
    return heat >= q2_limit(caliber, q0)


def cooling_per_tick(per_second: float) -> float:
    return per_second / COOLING_HZ


def sentry_supply_rounds(elapsed: float) -> int:
    """哨兵每分钟可在补给区领 100 发，未领的累计。"""
    return int(max(0.0, elapsed) // 60) * SENTRY_SUPPLY_ROUNDS


def card_damage(max_hp: int, fraction: float) -> int:
    """黄牌扣血；手册未写取整方式，按四舍五入，且至少 1 点。"""
    return max(1, round_half_up(max_hp * fraction))
