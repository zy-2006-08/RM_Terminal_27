"""自机 Buff(2.2.14)和敌方能量机关事件:模拟器发给终端的增益要和规则一致。

    .venv/bin/python -m sim.test_buffs
"""

import sim.match_server as ms
from sim.match_server import SELF_ROBOT_ID, SimulatedMatch

ms._COUNTDOWN_SEC = 4
_MATCH_AT = ms._PREP_SEC + ms._CHECK_SEC + ms._COUNTDOWN_SEC
HERO = ms._SELF_LOCAL_ID == 1


def at_match_second(seconds: float) -> SimulatedMatch:
    m = SimulatedMatch()
    m.reset()
    m.seek(_MATCH_AT + seconds)
    m.hold()
    m.at_exchange_point = False
    assert m.match_elapsed() == seconds, m.match_elapsed()
    return m


def advance(m: SimulatedMatch, seconds: float) -> None:
    m.seek(m.elapsed + seconds)
    m.hold()


def of_type(m: SimulatedMatch, buff_type: int) -> list[tuple[int, int, int]]:
    return [(level, longest, left) for t, level, longest, left in m.self_buffs() if t == buff_type]


# 地形(5.6.4.5):过高地防御 25% 30 秒;拿着它再过公路,防御变 50%,时长取较大的。
m = at_match_second(200)
assert m.gain_terrain(1) is None
assert of_type(m, 2) == [(25, 30, 30)], of_type(m, 2)
advance(m, 10)
assert m.gain_terrain(2) is None
assert of_type(m, 2) == [(50, 20, 20)], of_type(m, 2)

# 穿越隧道:1.5 倍热量冷却,协议发「每秒多冷却多少」,120 秒。
assert m.gain_terrain(4) is None
cooling = of_type(m, 3)
assert cooling == [(round(m.cooling_rate * 0.5), 120, 120)], cooling

# 踩了高地下面的卡还没踩上面:类型 7。
assert m.gain_terrain(5) is None
assert of_type(m, 7), m.self_buffs()
assert m.gain_terrain(9) is not None, "没有这种地形增益"

# 阵亡清掉地形增益,复活后也不会回来。
m.set_hp(SELF_ROBOT_ID, 0)
assert m.self_buffs() == [], m.self_buffs()
m._revive(SELF_ROBOT_ID, paid=True)
assert of_type(m, 2) == [] and of_type(m, 3) == [], m.self_buffs()
assert m.gain_terrain(1) is None
m.set_hp(SELF_ROBOT_ID, 0)
assert m.gain_terrain(1) is not None, "阵亡时拿不到"

# 我方小能量机关:防御 25%,45 秒。
m = at_match_second(100)
m.launcher_verified = True
m.rune_status, m.rune_big, m.rune_until = 3, False, 100 + 45
assert of_type(m, 2) == [(25, 45, 45)], m.self_buffs()

# 重装前 3 分钟、没装上 42mm:防御 50%,到 3:00(5.7.1)。
if HERO:
    m = at_match_second(100)
    m.launcher_verified = False
    assert of_type(m, 2) == [(50, 180, 80)], m.self_buffs()
    m.launcher_verified = True
    assert of_type(m, 2) == [], "装上 42mm 就没有了"
    m = at_match_second(181)
    m.launcher_verified = False
    assert of_type(m, 2) == [], "3 分钟后就没有了"

# 我方大能量机关(模拟的是 10 臂、平均 9.2 环):攻击 300%、防御 50%、冷却 5 倍,60 秒。
m = at_match_second(200)
m.rune_status, m.rune_big, m.rune_until = 3, True, 200 + 60
assert of_type(m, 1) == [(300, 60, 60)], m.self_buffs()
assert of_type(m, 2) == [(50, 60, 60)], m.self_buffs()
assert of_type(m, 3) == [(round(m.cooling_rate * 4), 60, 60)], m.self_buffs()

# 站在兑换点上:类型 6,和别的增益一起发。
m.at_exchange_point = True
assert of_type(m, 6) == [(0, 0, 0)], m.self_buffs()

# 敌方开能量机关:只发事件 3/4,我方能量机关状态不动。
m = at_match_second(60)
before = m.rune_status
assert m.enemy_rune(False) is None
assert m.events[-1] == (4, "1"), m.events
assert m.enemy_rune(True) is None
assert m.events[-2:] == [(3, ms._ENEMY_BIG_RUNE_EVENT), (4, "2")], m.events
assert m.rune_status == before
m = SimulatedMatch()
m.reset()
assert m.enemy_rune(False) is not None, "赛外不发"

print("sim buffs: all checks passed")
