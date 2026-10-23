import time

import sim.match_server as ms
from sim.match_server import SimulatedMatch

ms._COUNTDOWN_SEC = 4
_MATCH_AT = ms._PREP_SEC + ms._CHECK_SEC + ms._COUNTDOWN_SEC

m = SimulatedMatch()
m.reset()
assert m.elapsed == 0.0

m.seek(100)
assert m.elapsed == 100.0
m.begin()
time.sleep(0.05)
assert 100.0 <= m.elapsed < 102.0

m.set_speed(10)
before = m.elapsed
time.sleep(0.15)
assert 1.0 <= m.elapsed - before < 3.0, m.elapsed - before

m.hold()
frozen = m.elapsed
time.sleep(0.05)
assert m.elapsed == frozen

m.seek(_MATCH_AT + 2)
assert m.stage() == (4, 418), m.stage()
print("clock ok")

m.set_hp(101, 123)
assert m.other_hp[101] == 123
m.set_hp(3, 9999)
assert m.hp == m.max_hp
assert m.set_hp(999, 100) is False
assert not m.auto_hp and not m.fortress_auto, "默认应全手动"
print("hp ok")

frozen_hp = (m.hp, m.red_base_hp, m.blue_base_hp,
             m.red_outpost_hp, m.blue_outpost_hp)
for _ in range(50):
    m.tick_10hz()
assert (m.hp, m.red_base_hp, m.blue_base_hp,
        m.red_outpost_hp, m.blue_outpost_hp) == frozen_hp, "全手动时血量不应自己变"
print("manual only ok")

m.set_structure_hp(0, 99999)
assert m.red_base_hp == 10000
m.set_structure_hp(2, 123)
assert m.red_outpost_hp == 123
assert m.set_structure_hp(4, 1) is False
print("structure ok")

m.set_fortress(1)
assert m.fortress_holder == 1 and not m.fortress_auto
m.set_fortress(3)
assert m.fortress_auto
print("fortress ok")

m.red_hits = 9
m.red_reinforce_chances = 2
assert m.use_reinforce(1) is not None
m.red_hits = 10
assert m.use_reinforce(1) is None and m.red_reinforce_chances == 1
assert m.use_reinforce(1) is None and m.red_reinforce_chances == 0
assert m.use_reinforce(1) is not None
print("reinforce ok")

m.trigger_blind(2, 4)
m.tick_blind()
assert m.blind_side == 2 and m.blind_target_type == 4
assert 0 < m.blind_remaining_ms <= 10000
m.clear_blind()
assert m.blind_side == 0
m.trigger_blind(1, 2)
assert m.blind_until[1] - m.blind_started[1] == 10.0
m.clear_blind()
m.trigger_blind(1, 2)
assert m.blind_until[1] - m.blind_started[1] == 5.0
print("blind ok")

m.clear_blind()
m.seek(_MATCH_AT + 35)
m.tick_blind()
assert m.blind_side == 0, "默认不跑自动致盲时刻表"

m.trigger_blind(1, 2)
m.tick_blind()
assert m.blind_side == 1
m.clear_blind()
m.tick_blind()
assert m.blind_side == 0, "解除后不应在旧自动窗口内重新致盲"
print("manual blind only ok")

m.hp = 0
m.zones = [2, 2, 0, 0]
m._zone_last = m.elapsed - 1.0
before = m.red_control_points
m.tick_10hz()
assert m.red_control_points == before + 1, "自机阵亡不应冻结控制度结算"
print("dead self still ticks zones ok")

m.hp = m.max_hp
m.zones = list(ms._INITIAL_ZONES)
m.zone_progress = [0.0] * len(m.zones)

m.seek(_MATCH_AT + 30)
m.target_locked = True
m.target_id = 101
m.other_hp[101] = 200
m.red_economy = 800
m.team_ammo_bought = {17: 0, 42: 0}
assert m.buy_ammo(17, 5) is not None, "不是最小兑换单位的整数倍"
assert m.buy_ammo(17, 300) is None
assert m.ammo == 300 and m.coin == 500
assert m.buy_ammo(42, 10) is not None, "17mm 机器人不能兑换 42mm"
m.hp = 0
assert m.buy_ammo(17, 10) == "机器人已阵亡"
assert m.shoot(17, 1) == "机器人已阵亡"
m.hp = m.max_hp
m.heat = 0.0
m.heat_locked = False
m.heat_permanent_lock = False
m._heat_last = m.elapsed
assert m.shoot(17, 10) is None
# 冷却优先 1 级 Q0=40：第 5 发把热量推到 50 后锁定，剩余 5 发不发射。
assert m.ammo == 295, m.ammo
assert m.heat == 50 and m.heat_locked and not m.heat_permanent_lock
assert m.other_hp[101] == 100
assert m.shoot(17, 1) == "射击热量超限"
# 规则要求冷却到 0 才解锁，不是降到 Q0 就解锁。
for _ in range(5):
    m.seek(m.elapsed + 1)
    m.tick_10hz()
assert m.heat == 0 and not m.heat_locked
assert m.shoot(17, 1) is None
# Q1 ≥ Q2（17mm 为 Q0+100）后当局永久锁定。
m.heat = 130
m.heat_locked = False
m.heat_permanent_lock = False
m._heat_last = m.elapsed
assert m.shoot(17, 1) is None
assert m.heat_permanent_lock
assert m.shoot(17, 1) == "发射机构已永久锁定"
assert m.set_module() == "只有重装机器人可安装模块化 42mm 发射机构"
print("shoot ok")
# 热量测试把时钟推到了 +35s，恢复到开闸窗口起点再测飞镖。
m.seek(_MATCH_AT + 30)
m._heat_last = m.elapsed

assert m.dart_open(1) is None
assert m.dart_shoot(1, 1) is not None, "开闸 7 秒内不能发射"
m.seek(_MATCH_AT + 37)
assert m.dart_shoot(1, 1) is None
assert m.blue_outpost_hp == 0, "击毁后前哨站应倒下"
assert m.zones[1] == 3, "完全控制被击毁后应降为临时控制（表 5-21）"
assert m.zone_progress[1] == ms._ZONE_PROGRESS_SEC
for _ in range(5):
    m.seek(m.elapsed + 1)
    m.tick_zones()
assert m.blue_outpost_hp == 500, "5s 未激活后应按击毁方重建 500 HP"
assert m.zones[1] == 2, "5s 未激活后应转为红方完全控制"
assert m.dart_shoot(1, 1) is not None, "冷却 15 秒"
m.seek(_MATCH_AT + 52)
assert m.dart_shoot(1, 2) is None
assert m.blue_base_hp == 9800
m.tick_blind()
assert m.blind_side == 2
print("dart ok")

m.set_structure_hp(1, 100)
m.seek(_MATCH_AT + 240)
assert m.dart_open(1) is None
m.seek(_MATCH_AT + 247)
assert m.dart_shoot(1, 5) is None
assert m.blue_base_hp == 0
assert m.red_score == 1 and m.round_winner == 1
assert m.stage()[0] == 5, m.stage()
print("round end ok")

assert m.shoot(17, 100) == "弹丸数量必须是 1-99"
assert m.shoot(17, 1) == "比赛未开始或已结束"
assert not m.set_zone(2, 4), "基地阵地不能被对方完全控制"
print("guards ok")

m2 = SimulatedMatch()
m2.seek(_MATCH_AT + 420)
m2.red_base_hp = 100
m2.blue_base_hp = 9000
m2.red_control_points = 30
m2.blue_control_points = 20
assert m2.stage()[0] == 5, m2.stage()
assert m2.round_winner == 1, "时间到应按累计控制度判胜"
print("timeout control first ok")

m4 = SimulatedMatch()
m4.seek(_MATCH_AT + 2)
m4.red_base_hp = m4.blue_base_hp = 0
m4._end_round_by_base_hp()
assert m4.round_winner == 0 and m4.round_end_reason == 1, "双基地同毁且血量相同应判平局"
print("simultaneous base destruction draw ok")

m3 = SimulatedMatch()
m3.seek(_MATCH_AT + 61)
m3.tick_10hz()
assert m3.red_economy == 900 and m3.blue_economy == 900
m3.seek(_MATCH_AT + 361)
m3.tick_10hz()
assert m3.red_economy == 1600 and m3.blue_economy == 1600
print("economy milestones ok")

m3.set_structure_hp(2, 0)
assert m3.set_zone(0, 4)
assert m3.red_outpost_hp == 500, "敌方完全控制被击毁的前哨阵地应重建 500 HP"
print("outpost rebuild ok")

# 蓝方完全控制时击毁蓝方前哨站：先转蓝方临时控制，5s 后红方完全控制并重建。
o = SimulatedMatch()
o.seek(_MATCH_AT + 2)
assert o.zones[1] == ms._ZONE_BLUE_FULL
assert o.set_structure_hp(3, 0)
assert o.blue_outpost_hp == 0
assert o.zones[1] == ms._ZONE_BLUE_TEMP
assert o.zone_progress[1] == ms._ZONE_PROGRESS_SEC
for _ in range(5):
    o.seek(o.elapsed + 1)
    o.tick_zones()
assert o.zones[1] == ms._ZONE_RED_FULL, "5s 后应由击毁方红方完全控制"
assert o.blue_outpost_hp == 500, "5s 后应重建 500 HP"
print("blue outpost manual destroy ok")

# 红方已完全控制蓝方前哨阵地时再次击毁：立即由红方完全控制并重建。
r = SimulatedMatch()
r.seek(_MATCH_AT + 2)
r.zones[1] = ms._ZONE_RED_FULL
assert r.set_structure_hp(3, 0)
assert r.zones[1] == ms._ZONE_RED_FULL, "已完全控制方不应被降为临时控制"
assert r.blue_outpost_hp == 500, "已完全控制方击毁后应立即重建 500 HP"
print("outpost destroy by controlling side ok")

# 规则 5.6.1：己方完全控制己方前哨阵地时，基地免疫普通伤害；飞镖不受影响。
d = SimulatedMatch()
d.seek(_MATCH_AT + 2)
d.zones[0] = ms._ZONE_RED_FULL
before = d.red_base_hp
assert d._damage_structure(1, 500) == 0 and d.red_base_hp == before
assert d._damage_structure(1, 500, bypass_defense=True) == 500
d.zones[0] = ms._ZONE_RED_TEMP
assert d._damage_structure(1, 500) == 500, "临时控制不提供防御增益"
print("base defense ok")

# 表 5-21：临时控制靠持续激活维持，5s 不再激活就退回完全控制。
e = SimulatedMatch()
e.seek(_MATCH_AT + 2)
assert e.set_zone(0, ms._ZONE_BLUE_TEMP)
for _ in range(4):
    e.seek(e.elapsed + 1)
    e.tick_zones()
assert e.zones[0] == ms._ZONE_BLUE_TEMP and e.zone_progress[0] == 1.0
e.seek(e.elapsed + 1)
e.tick_zones()
assert e.zones[0] == ms._ZONE_RED_FULL, "5s 未激活应退回红方完全控制"
print("temp control decay ok")

# 表 5-21：持续激活只维持临时控制，升级完全控制必须击毁前哨站。
f = SimulatedMatch()
f.seek(_MATCH_AT + 2)
assert f.set_zone(1, ms._ZONE_RED_TEMP)
for i in range(1, 61):
    f.seek(f.elapsed + 0.1)
    if i % 4 == 0:
        assert f.set_zone(1, ms._ZONE_RED_TEMP)
    f.tick_zones()
assert f.zones[1] == ms._ZONE_RED_TEMP, "持续激活不应把临时控制升成完全控制"
print("temp control holds without promotion ok")

p = SimulatedMatch()
p.reset()
assert p.stage()[0] == 0, "复位后应停在赛外"
p.prep()
assert p.stage()[0] == 1, "prep 应进入准备阶段"
p.seek(ms._PREP_SEC)
assert p.stage()[0] == 2, "准备结束应进自检"
p.seek(ms._PREP_SEC + ms._CHECK_SEC)
assert p.stage()[0] == 3, "自检结束应进倒计时"
p.seek(_MATCH_AT)
assert p.stage()[0] == 4, "倒计时结束应进比赛"
print("stage flow ok")

q = SimulatedMatch()
q.seek(_MATCH_AT)
q.zones = [2, 2, 3, 4]
q.zone_progress = [0.0, 0.0, ms._ZONE_PROGRESS_SEC, 0.0]
q._zone_last = q.elapsed - 1.0
q._zone_accum = 0.0
q.tick_10hz()
assert (q.red_control_points, q.blue_control_points) == (1, 3), (
    q.red_control_points, q.blue_control_points)
assert not q.set_zone(0, 0), "前哨阵地不能未控"
print("control combo ok")
