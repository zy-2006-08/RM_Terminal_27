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
assert m.fortress_attack == 1 and not m.fortress_auto
m.set_fortress(3)
assert m.fortress_auto
m.set_fortress(0)
assert m.fortress_attack == 0 and not m.fortress_auto
assert m.set_fortress(4) is False
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
assert m.dart_open(1) == "闸门已经开着"
m.seek(_MATCH_AT + 44)
before = m.blue_base_hp
assert m.dart_shoot(1, 2) is None, "两发飞镖之间没有冷却(规则 5.7.3 的 15 秒是关闸后的)"
assert m.blue_base_hp == before - 200
m.tick_blind()
assert m.blind_side == 2
print("dart ok")

d = SimulatedMatch()
d.seek(_MATCH_AT + 20)
assert d.dart_open(1) == "10 秒后才有开闸机会"
d.seek(_MATCH_AT + 90)
assert d.dart_open(1) is None, "0:30 的机会没用就留着,1:30 也能开"
d.seek(_MATCH_AT + 130)
assert d.dart_open(1) == "110 秒后才有开闸机会", "0:30 那次已经用了"

d = SimulatedMatch()
d.seek(_MATCH_AT + 250)
assert d.dart_open(1) is None, "两次机会都没用就攒着"
d.seek(_MATCH_AT + 295)
assert d.dart_open(1) == "飞镖发射站冷却中，剩余 7.0s", "关闸后冷却 15 秒"
d.seek(_MATCH_AT + 303)
assert d.dart_open(1) is None
d.seek(_MATCH_AT + 350)
assert d.dart_open(1) == "飞镖闸门开启次数已达上限"
print("dart gate chances ok")

# 规则 5.7.3:每局 4 枚。中镖和开闸发射用的是同一份,用完了都不再命中,剩下的开闸机会也不让用。
c = SimulatedMatch()
c.seek(_MATCH_AT + 30)
assert c.trigger_blind(2, 2) is None and c.trigger_blind(2, 3) is None
assert c.dart_open(1) is None
c.seek(_MATCH_AT + 38)
assert c.dart_shoot(1, 1) is None and c.dart_shoot(1, 2) is None
assert c.dart_shoot(1, 3) == "本局飞镖已用完"
assert c.trigger_blind(2, 4) == "红方本局飞镖已用完"
assert sum(1 for kind, param in c.events if kind == 9 and param.startswith("1,")) == 4
c.seek(_MATCH_AT + 250)
assert c.dart_open(1) == "本局飞镖已用完"
assert c.trigger_blind(1, 2) is None, "蓝方的 4 枚另算"
print("darts per round ok")

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

b = SimulatedMatch()
b.seek(_MATCH_AT + 2)
assert b.trigger_blind(2, 1) is None and b.blue_outpost_hp == 0, "中镖前哨站扣 1000"
outpost_events = sum(1 for kind, _ in b.events if kind == 2)
assert b.trigger_blind(2, 1) is None and b.blue_outpost_hp == 0
assert sum(1 for kind, _ in b.events if kind == 2) == outpost_events, "倒下的前哨站不能再被击毁一次"
b.zones[1] = ms._ZONE_BLUE_FULL
for target, base_after in ((2, 9800), (3, 9500)):
    assert b.trigger_blind(2, target) is None and b.blue_base_hp == base_after, "飞镖不吃防御增益"
assert b.trigger_blind(2, 4) == "红方本局飞镖已用完" and b.blue_base_hp == 9500, "红方 4 枚用完,第 5 发不算"
b = SimulatedMatch()
b.seek(_MATCH_AT + 2)
assert b.trigger_blind(2, 4) is None and b.blue_base_hp == 9200
assert (b.other_hp[101], b.other_hp[103], b.other_hp[107]) == (320, 160, 320), \
    "随机移动目标：地面机器人各扣上限 20%"
assert b.hp == b.max_hp, "打的是蓝方，红方自机不扣"
assert b.trigger_blind(2, 5) is None and b.blue_base_hp == 7950
assert (b.other_hp[101], b.other_hp[103], b.other_hp[107]) == (120, 60, 120), "末端移动目标：各扣上限 50%"
assert b.trigger_blind(1, 4) is None and b.red_base_hp == 9200
assert b.hp == b.max_hp - int(b.max_hp * 0.2), "红方中镖，红方自机也扣 20%"
idle = SimulatedMatch()
idle.reset()
assert idle.trigger_blind(2, 2) is None and idle.blue_base_hp == 10000, "赛外中镖不扣血"
print("blind damage ok")

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

# 表 5-21:红方先临时控制蓝方前哨阵地(站满 5 秒),再打掉前哨站:立即红方完全控制并重建成红方的。
r = SimulatedMatch()
r.seek(_MATCH_AT + 2)
assert r.set_zone(1, ms._ZONE_RED_TEMP)
assert r.set_structure_hp(3, 0)
assert r.zones[1] == ms._ZONE_RED_FULL, "临时控制方击毁前哨站应立即完全控制"
assert r.blue_outpost_hp == 500 and r.outpost_owner[1] == ms._FACTION_RED, "当场重建成红方的 500 血"
print("outpost destroy by temporary controller ok")

# 被占的前哨站能抢回来:蓝方打掉红方前哨站、5 秒后重建成蓝方的;红方站满 5 秒再打掉,马上变回红方的。
t = SimulatedMatch()
t.seek(_MATCH_AT + 2)
assert t.set_structure_hp(2, 0)
assert t.zones[0] == ms._ZONE_RED_TEMP and t.red_outpost_hp == 0
t._expire_temp_control(0)
assert t.zones[0] == ms._ZONE_BLUE_FULL and t.outpost_owner[0] == ms._FACTION_BLUE and t.red_outpost_hp == 500
assert t.set_zone(0, ms._ZONE_RED_TEMP), "红方可以站在蓝方占的前哨站点上"
assert t.set_structure_hp(2, 0)
assert t.zones[0] == ms._ZONE_RED_FULL, "红方打掉蓝方占的前哨站,马上红方完全控制"
assert t.outpost_owner[0] == ms._FACTION_RED and t.red_outpost_hp == 500, "重建成红方的 500 血"
# 蓝方再占回去时也是一样:红方完全控制下被蓝方打掉,先红方临时。
assert t.set_structure_hp(2, 0)
assert t.zones[0] == ms._ZONE_RED_TEMP and t.red_outpost_hp == 0
print("outpost retake ok")

# 控制台直接把前哨阵地设成对方完全控制:按规则只能是前哨站被打掉再重建成对方的 500 血。
z = SimulatedMatch()
z.seek(_MATCH_AT + 2)
z.events.clear()
assert z.set_zone(1, ms._ZONE_RED_FULL)
assert z.blue_outpost_hp == 500 and z.outpost_owner[1] == ms._FACTION_RED
assert (2, "111") in z.events, "当作前哨站被打掉,发 Event 2"
assert "500 血" in z.note
assert z.set_zone(1, ms._ZONE_RED_TEMP) and z.zones[1] == ms._ZONE_RED_FULL, "红方已完全控制,再激活不变"
assert z.set_zone(1, ms._ZONE_BLUE_TEMP) and z.zones[1] == ms._ZONE_BLUE_TEMP, "蓝方能站回自己一侧的点上"
assert z.outpost_owner[1] == ms._FACTION_RED, "站在点上不换主人"
w = SimulatedMatch()
w.seek(_MATCH_AT + 2)
assert w.set_zone(0, ms._ZONE_BLUE_TEMP), "蓝方站上红方前哨站的点"
assert not w.set_zone(0, ms._ZONE_RED_TEMP), "前哨站还在且归红方时,红方不会是临时控制"
assert "先被对方打掉" in w.note
assert w.set_zone(0, ms._ZONE_RED_FULL) and w.zones[0] == ms._ZONE_RED_FULL and w.red_outpost_hp == 1000, \
    "蓝方走开:回到红方完全控制,前哨站原样"
assert w.set_structure_hp(2, 0) and not w.set_zone(0, ms._ZONE_BLUE_TEMP), "红方前哨站倒了,蓝方只能直接完全控制"
assert not w.set_structure_hp(2, 300), "倒下的前哨站不能直接加血"
assert w.red_outpost_hp == 0
print("control panel zone rules ok")

# 基地有 100% 防御时「设定血量」照改,但提醒这不是车打的。
h = SimulatedMatch()
h.seek(_MATCH_AT + 2)
assert h.set_structure_hp(0, 500) and h.red_base_hp == 500
assert "100% 防御" in h.note
h.note = ""
assert h.set_zone(0, ms._ZONE_BLUE_TEMP) and h.set_structure_hp(0, 400)
assert "100% 防御" not in h.note, "红方前哨阵地被蓝方临时控制时基地没有防御,不提醒"
print("base hp warning ok")

# 随机掉血打掉前哨站,也按主人的对手打的算。
a = SimulatedMatch()
a.seek(_MATCH_AT + 2)
a.outpost_owner[0] = ms._FACTION_BLUE
a.zones[0] = ms._ZONE_RED_TEMP
a.red_outpost_hp = 1
a.auto_hp = True
for _ in range(2000):
    if a.red_outpost_hp != 1:
        break
    a.tick_field_hp()
assert a.zones[0] == ms._ZONE_RED_FULL and a.outpost_owner[0] == ms._FACTION_RED, "随机掉血里也能抢回来"
print("random outpost damage ok")

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


def advance(match, seconds):
    # 不用 seek:它按整场赛程的绝对时刻推阶段,倒计时之后会被拉回第一局的开局时刻。
    match.hold_at = match.elapsed + seconds
    match.t0 = time.monotonic()


n = SimulatedMatch()
n.seek(_MATCH_AT + 420)
assert n.stage()[0] == 5 and n.round_winner == 0, "各项持平,时间到应判平局"
for round_index, score in ((1, (1, 0)), (2, (2, 0))):
    n.countdown()
    advance(n, ms._COUNTDOWN_SEC + 0.1)
    assert n.stage()[0] == 4 and n.round_index == round_index, (n.stage(), n.round_index)
    assert n.winner() == 255 and n.blue_base_hp == ms._BASE_MAX_HP, "结算中点倒计时应清掉上一局的结果和血量"
    assert n.set_structure_hp(1, 0)
    assert n.stage()[0] == 5 and n.winner() == 1, "打爆蓝方基地应判红方胜"
    assert (n.red_score, n.blue_score) == score, (n.red_score, n.blue_score)
assert n.match_over
n.countdown()
assert n.stage()[0] == 3 and n.round_index == 0 and (n.red_score, n.blue_score) == (0, 0), \
    "整场打完再点倒计时应从第一局 0:0 重来"
print("countdown during settlement starts next round ok")
