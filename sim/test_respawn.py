"""复活状态机回归：死亡读条、买活、虚弱/无敌、交互卡、空中排除。"""

import sim.match_server as ms
from sim import rules
from sim.match_server import SELF_ROBOT_ID, SimulatedMatch

ms._COUNTDOWN_SEC = 4
_MATCH_AT = ms._PREP_SEC + ms._CHECK_SEC + ms._COUNTDOWN_SEC

m = SimulatedMatch()
m.reset()
m.seek(_MATCH_AT + 60)
m.hold()
assert m.match_elapsed() == 60.0, m.match_elapsed()

# 死亡：读条 10 + 60/10 = 16s；清热量和临时锁，永久锁保留。
m.heat = 120.0
m.heat_locked = True
m.heat_permanent_lock = True
m.set_hp(SELF_ROBOT_ID, 0)
st = m.respawn_state[SELF_ROBOT_ID]
assert st["dead"] and st["total"] == 16, st
assert m.heat == 0.0 and not m.heat_locked and m.heat_permanent_lock
assert not m.is_weak(SELF_ROBOT_ID)

pending, total, current, free, cost, can_pay = m.respawn_status()
assert pending and total == 16 and current == 0 and not free, (pending, total, current)
assert cost == 100 and can_pay == (100 <= m.coin), (cost, can_pay, m.coin)

# 买活：elapsed 60s 花 100 金币，满血，3s 虚弱+无敌。
coins = m.coin
assert m.pay_revive() is None
assert m.coin == coins - 100, m.coin
assert m.hp == m.max_hp and st["paid"] == 1
assert m.is_weak(SELF_ROBOT_ID) and m.is_invincible(SELF_ROBOT_ID)
assert st["weak_until"] == 63.0 and st["invincible_until"] == 63.0, st
assert not m.respawn_status()[0]

# 虚弱锁发射机构。
m.heat_permanent_lock = False
m.ammo = 10
m.target_locked = True
m.target_id = 101
m.other_hp[101] = 100
assert m.shoot(17, 1) == "复活虚弱中，发射机构锁定"
assert m.ammo == 10

# 买活刷卡的交互卡只解虚弱，不缩短 3s 无敌；之后再刷无效。
assert m.card_scan() is None
assert not m.is_weak(SELF_ROBOT_ID)
assert st["invincible_until"] == 63.0, st
assert m.card_scan() == "机器人未处于虚弱状态"

# 无敌目标不能受击。
m.respawn_state[101]["invincible_until"] = 70.0
assert m.shoot(17, 1) == "锁定目标处于无敌状态"
assert m.other_hp[101] == 100

# 免费复活：买活次数让读条 +20s；基地 <4000 时读条 4/s。
m.set_hp(SELF_ROBOT_ID, 0)
assert st["total"] == 36, st
m.red_base_hp = 3999
m._respawn_last = m.elapsed - 1.0
before = st["progress"]
m.tick_respawn()
assert st["progress"] == before + 4.0, st

st["progress"] = st["total"] - 1.0
pending, total, current, free, cost, _ = m.respawn_status()
assert pending and current == total - 1 and not free, (pending, total, current)
st["progress"] = st["total"]
assert m.respawn_status()[3]
m._respawn_last = m.elapsed - 1.0
m.tick_respawn()
assert not st["dead"]
assert m.hp == max(1, rules.round_half_up(m.max_hp * rules.REVIVE_HP_FRACTION))
assert m.is_weak(SELF_ROBOT_ID) and m.is_invincible(SELF_ROBOT_ID)
assert st["weak_until"] - st["revived_at"] == 30.0, st

# 免费复活刷卡：解虚弱，无敌可提前结束但至少累计 10s。
assert m.card_scan() is None
assert not m.is_weak(SELF_ROBOT_ID)
assert st["invincible_until"] == st["revived_at"] + 10.0, st

# 空中机器人不适用地面复活读条。
m.set_hp(106, 0)
air = m.respawn_state[106]
assert m.other_hp[106] == 0 and not air["dead"]
m._respawn_last = m.elapsed - 1.0
m.tick_respawn()
assert m.other_hp[106] == 0 and not air["dead"]

# 下一局清空读条与买活次数。
m.start_next_round()
st = m.respawn_state[SELF_ROBOT_ID]
assert not st["dead"] and st["paid"] == 0
assert m.hp == m.max_hp
assert not m.respawn_status()[0]

print("respawn ok")
