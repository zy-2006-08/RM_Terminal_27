"""2027 协议换算回归:模拟器发给终端的「己方/对方」编码、排列顺序、单位和 Event 参数。

终端按 cpp/decoder.cpp 的规则把这些换回红蓝,两边任何一处对不上,终端就会把敌我画反。
这里只测纯函数和 SimulatedMatch 的事件账本,不起 MQTT。
"""

import sim.match_server as ms
from sim.match_server import (
    SimulatedMatch, base_status_code, dart_blind_duration, max_assembly_difficulty,
    ordered_robot_ids, radar_centimetres, radar_robot_ids, relative_base_shield,
    relative_outpost_status, zone_event,
)

RED, BLUE = ms._FACTION_RED, ms._FACTION_BLUE
R_TEMP, R_FULL, B_TEMP, B_FULL = (ms._ZONE_RED_TEMP, ms._ZONE_RED_FULL,
                                  ms._ZONE_BLUE_TEMP, ms._ZONE_BLUE_FULL)

# 前哨阵地(2.2.4 字段 5/10):1 己方完全 2 己方临时 3 对方临时 4 对方完全。
assert [relative_outpost_status(z, RED) for z in (0, R_TEMP, R_FULL, B_TEMP, B_FULL)] == [0, 2, 1, 3, 4]
assert [relative_outpost_status(z, BLUE) for z in (0, R_TEMP, R_FULL, B_TEMP, B_FULL)] == [0, 3, 4, 2, 1]
# 己方基地阵地(字段 3):1 己方完全 2 对方临时;对方基地阵地(字段 8):1 对方完全 2 己方临时。
assert relative_base_shield(R_FULL, RED, False) == 1 and relative_base_shield(B_TEMP, RED, False) == 2
assert relative_base_shield(B_FULL, RED, True) == 1 and relative_base_shield(R_TEMP, RED, True) == 2
assert relative_base_shield(B_FULL, BLUE, False) == 1 and relative_base_shield(R_TEMP, BLUE, False) == 2
assert relative_base_shield(R_FULL, BLUE, True) == 1 and relative_base_shield(B_TEMP, BLUE, True) == 2
print("relative zone codes ok")

# robot_health:己方 1,3,4,7,对方 1,3,4,7;雷达:对方在前、己方在后。
assert ordered_robot_ids(RED) == (1, 3, 4, 7, 101, 103, 104, 107)
assert ordered_robot_ids(BLUE) == (101, 103, 104, 107, 1, 3, 4, 7)
assert radar_robot_ids(RED) == (101, 103, 104, 107, 1, 3, 4, 7)
assert radar_robot_ids(BLUE) == (1, 3, 4, 7, 101, 103, 104, 107)
assert radar_centimetres(14.567) == 1457 and radar_centimetres(-0.3) == 0
print("orderings and units ok")

# 规则 5.7.3:固定类第 1-4 次 10/5/3/2 秒,之后按 2 秒;移动类每次 10 秒。
assert [dart_blind_duration(n, 2) for n in (1, 2, 3, 4, 5)] == [10.0, 5.0, 3.0, 2.0, 2.0]
assert dart_blind_duration(3, 4) == 10.0 and dart_blind_duration(1, 5) == 10.0
assert [base_status_code(True, True), base_status_code(False, False), base_status_code(False, True)] == [0, 1, 2]
assert [max_assembly_difficulty(t) for t in (0, 59, 60, 125, 180, 400)] == [1, 1, 2, 3, 4, 4]
print("rule tables ok")

# Event 5-8:前哨阵地参数就是绝对编码,基地阵地只有「完全/临时」两种。
assert zone_event(0, B_TEMP) == (5, "3") and zone_event(1, R_FULL) == (6, "2")
assert zone_event(2, R_FULL) == (7, "1") and zone_event(2, B_TEMP) == (7, "2")
assert zone_event(3, B_FULL) == (8, "1") and zone_event(3, R_TEMP) == (8, "2")
assert zone_event(2, B_FULL) is None and zone_event(0, 0) is None
print("zone events ok")

_MATCH_AT = ms._PREP_SEC + ms._CHECK_SEC + ms._COUNTDOWN_SEC


def match_at(seconds: float) -> SimulatedMatch:
    m = SimulatedMatch()
    m.seek(_MATCH_AT + seconds)
    m.hold()
    m.events.clear()
    return m


# 飞镖命中:Event 9 的参数是「命中方,目标」;trigger_blind 的第一个参数是被命中的一方。
m = match_at(40)
m.trigger_blind(RED, 4)
assert (9, "2,4") in m.events, m.events
assert ((13, "") in m.events) == (ms._ENEMY_FACTION == RED), "只有对方护甲展开才报 Event 13"
assert m.blind_until[RED] - m.blind_started[RED] == 10.0
assert m.armor_open[RED], "移动类目标命中一发护甲就展开"
m.events.clear()
for _ in range(4):
    m.trigger_blind(BLUE, 2)
assert all(event == (9, "1,2") for event in m.events if event[0] == 9)
assert m.armor_open[BLUE], "固定目标 4 发全中,护甲展开"
assert m.blind_until[BLUE] - m.blind_started[BLUE] == 10.0 + 5.0 + 3.0 + 2.0, "连续命中时长叠加"
print("dart events ok")

# 击杀:Event 1「被击杀者,击杀者」。
m = match_at(60)
m.target_locked, m.target_id = True, 101 if ms._SELF_FACTION == RED else 1
# 重装(42mm 未装)和指挥手(空中)没有 17mm 发射机构,这里只借 shoot() 的击杀路径。
m.launcher_ready, m.caliber = True, 17
m.ammo = 50
m.other_hp[m.target_id] = 20
assert m.shoot(17, 1) is None
assert (1, f"{m.target_id},{ms.SELF_ROBOT_ID}") in m.events, m.events
print("kill event ok")

# 前哨站被击毁:Event 2 + 阵地易手 Event 6(B 前哨阵地变成蓝方临时)。
m = match_at(60)
assert m.set_structure_hp(3, 0)
assert (2, "111") in m.events and (6, str(B_TEMP)) in m.events, m.events
m.events.clear()
assert m.set_zone(1, B_TEMP)
assert m.events == [], "状态没变不发事件"
print("outpost events ok")

# 堡垒:被对方激活满 10 秒,这一方的护甲展开;对方护甲展开时发 Event 13。
m = match_at(200)
assert m.set_fortress(ms._ENEMY_FACTION)
for second in range(1, 12):
    m.seek(m.elapsed + 1)
    m.tick_fortress()
assert m.fortress_activation[ms._ENEMY_FACTION] == 10.0
assert m.armor_open[ms._ENEMY_FACTION] and (13, "") in m.events
print("fortress armor ok")

# 我方基地掉血:Event 11,5 秒内只发一次。
m = match_at(60)
m.set_structure_hp(0 if ms._SELF_FACTION == RED else 1, 9000)
m.set_structure_hp(0 if ms._SELF_FACTION == RED else 1, 8000)
assert m.events.count((11, "")) == 1, m.events
print("base attacked ok")

# 每一个待发事件都是 (编号, 参数),编号在协议 1–16 之内。
for event_id, param in m.events:
    assert 1 <= event_id <= 16 and isinstance(param, str)
print("test_protocol_2027: OK")
