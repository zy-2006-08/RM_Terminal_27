"""sim/rules.py 的断言测试：只用标准库，`python -m sim.test_rules` 直接跑。"""

from __future__ import annotations

from sim import rules


def test_level_and_exp() -> None:
    assert rules.level_for_exp(0) == 1
    assert rules.level_for_exp(549) == 1
    assert rules.level_for_exp(550) == 2
    assert rules.level_for_exp(4999) == 9
    assert rules.level_for_exp(5000) == 10
    # 表 5-14 示例一：2 级击毁 6 级 → 540。
    assert rules.kill_exp(6, 2) == 540
    # 被击毁者等级不高于击毁者时没有等级差加成。
    assert rules.kill_exp(2, 6) == 100
    assert rules.damage_exp("robot", 120) == 480
    assert rules.damage_exp("outpost", 100) == 200
    assert rules.damage_exp("base", 3) == 2
    assert rules.damage_exp("base", 4) == 2


def test_logistics_and_revive() -> None:
    assert rules.logistics_lambda(10000) == 1.0
    assert rules.logistics_lambda(8000) == 0.9
    assert rules.logistics_lambda(0) == 0.5
    # 手册示例：剩余 120s、已兑换 2 次 → 80s。
    assert rules.revive_delay(120, 2, 1.0) == 80
    # 读条除以 λ：基础 10 秒在 λ=0.5 时变成 20 秒。
    assert rules.revive_delay(420, 0, 0.5) == 20
    assert rules.revive_rate(True, 10000) == 4.0
    assert rules.revive_rate(False, 3999) == 4.0
    assert rules.revive_rate(False, 4000) == 1.0
    # 表 5-7：立即复活 = ROUNDUP((420-剩余)/60)*80 + 等级*20。
    assert rules.pay_revive_cost(420, 1) == 20
    assert rules.pay_revive_cost(360, 1) == 100
    assert rules.pay_revive_cost(120, 2) == 440


def test_healing() -> None:
    # 补给区 10%，比赛满 4 分钟且脱战 25%，都要乘 λ。
    assert rules.supply_regen_rate(200, 100, True, 1.0) == 20
    assert rules.supply_regen_rate(200, 239, True, 1.0) == 20
    assert rules.supply_regen_rate(200, 240, True, 1.0) == 50
    assert rules.supply_regen_rate(200, 240, False, 1.0) == 20
    assert rules.supply_regen_rate(200, 240, True, 0.9) == 45
    assert rules.outpost_regen_rate(200, 100, True, 1.0) == 10
    # 远程回血：20 在取整括号内，所以剩余 359s 时是 50+21 而不是 50+20。
    assert rules.remote_heal_cost(420) == 50
    assert rules.remote_heal_cost(359) == 71
    assert rules.remote_heal_cost(300) == 90
    assert rules.remote_heal_amount(200, 0.9) == 108


def test_ammo_and_heat() -> None:
    assert rules.ammo_exchange_cost(17, 100, True) == 150
    assert rules.ammo_exchange_cost(17, 10, False) == 10
    assert rules.ammo_exchange_cost(42, 10, True) == 150
    assert rules.ammo_exchange_cost(42, 1, False) == 10
    assert rules.ammo_exchange_cost(17, 150, True) is None
    assert rules.ammo_exchange_cost(19, 10, True) is None
    assert rules.AMMO_CAP == {17: 1000, 42: 100}
    # 17mm Q2 = Q0+100，42mm Q2 = Q0+200。
    assert rules.q2_limit(17, 40) == 140
    assert rules.q2_limit(42, 300) == 500
    assert not rules.heat_locked(40, 40)
    assert rules.heat_locked(50, 40)
    assert not rules.heat_permanently_locked(139, 40, 17)
    assert rules.heat_permanently_locked(140, 40, 17)
    assert rules.cooling_per_tick(12) == 1.2
    assert rules.sentry_supply_rounds(185) == 300


def test_cards_and_tables() -> None:
    assert rules.card_damage(200, rules.CARD_OFFENDER_FRACTION) == 30
    assert rules.card_damage(400, rules.CARD_TEAMMATE_FRACTION) == 20
    assert rules.card_damage(1, rules.CARD_TEAMMATE_FRACTION) == 1
    assert rules.CHASSIS_HP_BLOOD[0] == 200 and rules.CHASSIS_HP_BLOOD[9] == 400
    assert rules.CHASSIS_HP_POWER[0] == 150 and rules.CHASSIS_HP_POWER[9] == 400
    assert rules.LAUNCHER_COOLING[0] == (40, 12)
    assert rules.LAUNCHER_BURST[9] == (260, 20)


def main() -> None:
    test_level_and_exp()
    test_logistics_and_revive()
    test_healing()
    test_ammo_and_heat()
    test_cards_and_tables()
    print("test_rules: OK (5 groups)")


if __name__ == "__main__":
    main()
