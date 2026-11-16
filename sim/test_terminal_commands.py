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

# 非远程兑换要站在兑换点上(5.4.2);远程兑换不用。
m = at_match_second(30)
m.launcher_ready = True
unit = rules.AMMO_EXCHANGE[(m.caliber, False)][0]
assert m.at_exchange_point, "模拟器默认车站在兑换点上"
m.at_exchange_point = False
assert m.buy_ammo(m.caliber, unit) == "未在兑换点"
assert m.ammo == 0
m._combat_at = None
assert m.remote_buy_ammo(rules.AMMO_EXCHANGE[(m.caliber, True)][0]) is None, "远程兑换不看兑换点"
m.at_exchange_point = True
assert m.buy_ammo(m.caliber, unit) is None and m.ammo == unit

# 测速模块(2.2.12):装着哪种发射机构亮哪个;没装都是 0。
m = at_match_second(30)
m.launcher_ready, m.caliber = True, 17
assert m.shooter_modules() == (1, 0)
m.caliber = 42
assert m.shooter_modules() == (0, 1)
m.launcher_ready = False
assert m.shooter_modules() == (0, 0)

# 累计已发弹量只算自机打出去的,兑换不算。
m = at_match_second(30)
m.launcher_ready, m.caliber = True, 17
assert m.buy_ammo(17, 50) is None and m.fired_total == 0, "兑换不是发射"

# 这一条命的受伤统计(2.2.8):飞镖算飞镖溅射,别的掉血算 17mm 弹丸;复活后清零。
m = at_match_second(40)
m._hp_seen = m.hp
m._write_hp(SELF_ROBOT_ID, m.hp - 30)
m.track_combat()
assert m.injury["small"] == 30 and m.injury["dart"] == 0, m.injury
m._pending_dart = 20
m._write_hp(SELF_ROBOT_ID, m.hp - 20)
m.track_combat()
assert m.injury["dart"] == 20 and m.injury["small"] == 30, m.injury
m.set_hp(SELF_ROBOT_ID, 0)
assert m.pay_revive() is None
assert sum(m.injury.values()) == 0, "复活后清零"

# 重装的 42mm(规则 5.7.1):有安装权限才装得上;装上能买弹,打出一发验证弹才算成功安装(Event 16)。
if ms._SELF_LOCAL_ID == 1:
    m = at_match_second(60)
    assert m.set_module().startswith("还没有安装权限"), "60 秒、没装成三级:还不能装"
    assert not m.launcher_ready and m.shooter_modules() == (0, 0)
    m.tech_level[ms._SELF_FACTION] = 3
    m.events.clear()
    assert m.set_module() is None and m.launcher_ready and not m.launcher_verified
    assert m.shooter_modules() == (0, 1), "装上后 42mm 测速模块在线,终端放开 42mm 兑换"
    assert (16, "") not in m.events, "只装上不发 Event 16"
    assert m.max_hp == 400, "成功安装前还是开局 400 血"
    assert m.buy_ammo(42, 2) is None and m.ammo == 2, "装上就能买 42mm"
    assert m.verify_launcher() is None
    assert m.launcher_verified and (16, "") in m.events and m.ammo == 1, "验证弹扣 1 发,发 Event 16"
    assert m.max_hp == 600 and m.hp == 600, "成功安装后上限 600(表 5-16)"
    assert not m.out_of_combat(), "打出验证弹就不算脱战"
    assert m.verify_launcher() is not None and m.set_module() is not None, "已经装好了,不能再装、再验"

    m = at_match_second(301)
    assert m.set_module() is None
    m.events.clear()
    assert m.shoot(42, 1) is None and m.launcher_verified and (16, "") in m.events, \
        "没点「打验证弹」直接射击:第一发就是验证弹"
    assert "超发" in m.note, "允许发弹量 0 时打出验证弹,提醒算超发"
    m.start_next_round()
    assert not m.launcher_ready and not m.launcher_verified and m.max_hp == 400, "下一局重新装"

# 下一局清掉远程兑换的待到账和能量机关用掉的次数。
m = at_match_second(60)
m.remote_pending.append((61.0, "ammo", 100, m.deaths))
m.rune_used["small"] = 1
m.fired_total = 30
m.start_next_round()
assert m.remote_pending == [] and m.rune_used == {"small": 0, "big": 0}
assert m.fired_total == 0, "累计已发弹量每局清零"

# 指挥手的哨兵指令按总价扣钱，和终端按钮上写的一样(规则 5.7.2、表 5-7)。
assert ms.sentry_command_cost(7, 100, True) == 50 and ms.sentry_command_cost(7, 100, False) == 0
assert ms.sentry_command_cost(3, 0, True) == 200, "远程 100 发 150 + 干预费 50"
assert ms.sentry_command_cost(4, 300, True) == 200, "5:00 远程回血 50 + 100 + 50"
assert ms.sentry_command_cost(6, 300, True) == 470, "5:00 立即复活 5×80 + 1 级×20 + 50"
assert ms.sentry_command_cost(5, 300, False) == 0, "半自动哨兵确认复活不花钱"

print("terminal commands ok")
