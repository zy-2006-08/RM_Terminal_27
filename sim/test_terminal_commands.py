"""终端发来的本车指令:兑换、远程兑换、复活确认、性能体系、能量机关机会。

    .venv/bin/python -m sim.test_terminal_commands            # 默认席位(红方 3 号步兵)
    RM_SELF_ID=101 .venv/bin/python -m sim.test_terminal_commands   # 重装
"""

import sim.match_server as ms
from sim import rules
from sim.match_server import SELF_ROBOT_ID, SimulatedMatch

ms._COUNTDOWN_SEC = 4
_MATCH_AT = ms._PREP_SEC + ms._CHECK_SEC + ms._COUNTDOWN_SEC
INFANTRY = ms._SELF_LOCAL_ID in (3, 4)


def at_match_second(seconds: float) -> SimulatedMatch:
    m = SimulatedMatch()
    m.reset()
    m.seek(_MATCH_AT + seconds)
    m.hold()
    assert m.match_elapsed() == seconds, m.match_elapsed()
    return m


def advance(m: SimulatedMatch, seconds: float) -> None:
    m.seek(m.elapsed + seconds)
    m.hold()


# 性能体系:只有步兵,准备阶段开始后、比赛开始前;不选时是 1/1(血量优先、冷却优先)。
m = SimulatedMatch()
m.reset()
assert (m.chassis_mode, m.shooter_mode) == (1, 1)
assert m.select_performance(2, 2) is not None, "赛外不能选"
m.seek(1.0)
m.hold()
assert m.stage()[0] == ms._STAGE_PREP
if INFANTRY:
    assert m.select_performance(2, None) is None and m.shooter_mode == 2 and m.chassis_mode == 1
    assert m.select_performance(None, 2) is None and m.chassis_mode == 2
    assert m.select_performance(3, 1) == "枚举值只能是 1 或 2"
else:
    assert m.select_performance(2, 2) == "只有步兵有性能体系"
m = at_match_second(10)
assert m.select_performance(1, 1) is not None, "七分钟比赛开始后锁定"

# 远程兑换:要脱战;金币当场扣,6 秒后到账。
m = at_match_second(60)
m.launcher_ready = True
coins = m.coin
unit, price = rules.AMMO_EXCHANGE[(m.caliber, True)]
m._combat_at = 58.0
assert m.remote_buy_ammo(unit).startswith("未脱战"), "交战中不能远程兑换"
m._combat_at = None
assert m.remote_buy_ammo(unit + 1).startswith("发数必须是"), "按最小单位兑换"
assert m.remote_buy_ammo(unit) is None
assert m.coin == coins - price and m.ammo == 0, (m.coin, m.ammo)
advance(m, 5.0)
m.tick_remote()
assert m.ammo == 0, "6 秒内不到账"
advance(m, 1.5)
m.tick_remote()
assert m.ammo == unit, m.ammo

# 远程回血:6 秒后加上限血量 60%×λ,不超上限;6 秒内阵亡作废、不退钱。
m = at_match_second(120)
m._write_hp(SELF_ROBOT_ID, 20)
m._hp_seen = m.hp
coins = m.coin
cost = rules.remote_heal_cost(rules.MATCH_SEC - 120)
assert m.remote_heal() is None and m.coin == coins - cost, (m.coin, coins, cost)
advance(m, 6.5)
m.tick_remote()
assert m.hp == min(m.max_hp, 20 + rules.remote_heal_amount(m.max_hp, 1.0)), m.hp
m._write_hp(SELF_ROBOT_ID, 20)
m._hp_seen = m.hp
assert m.remote_heal() is None
m.set_hp(SELF_ROBOT_ID, 0)
assert m.pay_revive() is None, "花钱复活,但远程回血那一单已经作废"
advance(m, 6.5)
m.tick_remote()
assert m.hp == m.max_hp, "立即复活满血;作废的回血没有再加"

# 交战判定:掉血算交战,6 秒后脱战。
m = at_match_second(200)
assert m.out_of_combat(), "开局默认脱战"
m._write_hp(SELF_ROBOT_ID, m.hp - 10)
m.track_combat()
assert not m.out_of_combat() and m.out_of_combat_countdown() == 6
advance(m, 6.0)
assert m.out_of_combat()
m._write_hp(SELF_ROBOT_ID, m.hp - 10)
m.track_combat()
m.set_hp(SELF_ROBOT_ID, 0)
advance(m, 10.0)
assert m.out_of_combat_countdown() == 0, "阵亡时倒计时是 0:协议字段是 uint32,负数会让发送线程崩掉"

# 能量机关机会:小能量机关 0 秒和 90 秒各一次可累积,3 分钟后换成大能量机关的三次。
m = at_match_second(10)
assert m.rune_chances() == 1
advance(m, 85)
assert m.rune_chances() == 2, "1 分 30 秒再给一次,可累积"
m.rune_used["small"] = 2
assert m.rune_chances() == 0
advance(m, 90)
assert m.match_elapsed() == 185.0 and m.rune_chances() == 1, "3 分钟后换大能量机关"
advance(m, 150)
assert m.rune_chances() == 3

# 下一局清掉远程兑换的待到账和能量机关用掉的次数。
m = at_match_second(60)
m.remote_pending.append((61.0, "ammo", 100, m.deaths))
m.rune_used["small"] = 1
m.start_next_round()
assert m.remote_pending == [] and m.rune_used == {"small": 0, "big": 0}

print("terminal commands ok")
