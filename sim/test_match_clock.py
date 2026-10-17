import time

from sim.match_server import SimulatedMatch

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

m.seek(6)
assert m.stage() == (4, 418), m.stage()
print("clock ok")

m.set_hp(101, 123)
assert m.other_hp[101] == 123
m.set_hp(3, 9999)
assert m.hp == m.max_hp
assert m.set_hp(999, 100) is False
print("hp ok")
