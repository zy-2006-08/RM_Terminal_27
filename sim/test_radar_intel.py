"""雷达情报(雷达情报格式.md):模拟器经 0x0310 带给终端的 81 字节要和格式文档一致。

    .venv/bin/python -m sim.test_radar_intel
"""

import struct

import sim.match_server as ms
from sim.match_server import SimulatedMatch

ms._COUNTDOWN_SEC = 4
_MATCH_AT = ms._PREP_SEC + ms._CHECK_SEC + ms._COUNTDOWN_SEC
ENEMY = sorted(ms._ENEMY_IDS, key=lambda rid: rid % 100)


def at_match_second(seconds: float) -> SimulatedMatch:
    m = SimulatedMatch()
    m.reset()
    m.seek(_MATCH_AT + seconds)
    m.hold()
    assert m.match_elapsed() == seconds, m.match_elapsed()
    return m


def robot(packet: bytes, index: int) -> tuple[int, int, int, int, int, int]:
    slot = (0, 14, 21, 28)[index]
    state = (36, 38, 39, 40)[index]
    return (*struct.unpack_from("<BHBBH", packet, 34 + slot), packet[34 + state])


# 比赛没开始不发。
m = SimulatedMatch()
m.reset()
assert m.radar_intel_packet() is None

# 第 100 秒:头、长度、发弹量顺序(1、3、4、保留、7),敌方重装前 3 分钟防御 50%,哨兵防御姿态。
m = at_match_second(100)
p = m.radar_intel_packet()
assert p is not None and len(p) == 81, len(p or b"")
assert p[:3] == b"RI\x01", p[:3]
assert struct.unpack_from("<H", p, 4)[0] == 1
assert struct.unpack_from("<5H", p, 12) == (17, 250, 220, 0, 400), struct.unpack_from("<5H", p, 12)
assert robot(p, 0) == (0, 0, 50, 0, 0, 0), robot(p, 0)
assert robot(p, 1) == (0, 0, 0, 0, 0, 0), robot(p, 1)
assert robot(p, 3)[2] == 50 and p[34 + 35] == 2
gold, total = struct.unpack_from("<HH", p, 22)
assert (gold, total) == (650, 900), (gold, total)

# 敌方开小能量机关:四台都防御 25%,重装取 50%;大能量机关攻击 300。
m.enemy_rune(False)
p = m.radar_intel_packet()
assert robot(p, 1)[2] == 25 and robot(p, 0)[2] == 50, (robot(p, 1), robot(p, 0))
m.enemy_rune(True)
p = m.radar_intel_packet()
assert robot(p, 2)[4] == 300 and robot(p, 2)[2] == 50, robot(p, 2)

# 阵亡 1,读条复活无敌且虚弱 3。
m.set_hp(ENEMY[1], 0)
assert robot(m.radar_intel_packet(), 1)[5] == 1
m._revive(ENEMY[1], False)
assert robot(m.radar_intel_packet(), 1)[5] == 3, robot(m.radar_intel_packet(), 1)

# 平时各在自己半场里绕;intrude 4 秒过中线,过的那一会儿雷达情报报我方这边的跨越点。
m = at_match_second(120)
for rid, faction, x, _y, _yaw in m.fabricated_other_poses():
    assert (x < 14) == (faction == ms._FACTION_RED), (rid, x)
assert m.intrude(ENEMY[1], "tunnel") is None
m.seek(m.elapsed + 2.0)
m.hold()
x, _y = m.scripted_pose(ENEMY[1], m.elapsed)
assert abs(x - 14.0) < 0.01, x
assert struct.unpack_from("<I", m.radar_intel_packet(), 30)[0] & (1 << 13)
m.seek(m.elapsed + 3.0)
m.hold()
assert not struct.unpack_from("<I", m.radar_intel_packet(), 30)[0] & 0x3FFFFF
assert m.intrude(ms._FRIENDLY_IDS[0], "tunnel") is not None

# 摆到我方堡垒上、敌方在偷我方堡垒:雷达情报里这台易伤 100。
red_self = ms._SELF_FACTION == ms._FACTION_RED
assert m.place(ENEMY[1], 6.57 if red_self else 21.43, 7.5) is None
assert m.set_fortress(ms._SELF_FACTION)
assert robot(m.radar_intel_packet(), 1)[3] == 100, robot(m.radar_intel_packet(), 1)
assert m.place(ENEMY[1], None, None) is None
assert robot(m.radar_intel_packet(), 1)[3] == 0

# 停发。
m.radar_intel_on = False
assert m.radar_intel_packet() is None

print("test_radar_intel: OK")
