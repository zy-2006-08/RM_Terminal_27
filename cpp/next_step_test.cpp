#include "next_step.h"

#include <cstdio>

namespace {

using rm_terminal::NextCard;
using rm_terminal::NextRule;
using rm_terminal::RobotId;
using rm_terminal::Snapshot;

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) return;
    std::fprintf(stderr, "FAIL %s\n", label);
    ++failures;
}

template <class T>
void set(rm_terminal::Field<T>& field, T value) {
    field.value = value;
    field.quality = rm_terminal::Quality::Valid;
    field.freshness = rm_terminal::Freshness::Fresh;
}

// 比赛第 200 秒:红方这台车活着、满血、弹量见底、800 金;双方基地都没防御(状态 1),
// 敌方 3 号残血且雷达看得到。每项测试再改自己关心的字段。
Snapshot match_for(std::uint32_t self_id, std::uint32_t ammo) {
    Snapshot s;
    s.self = RobotId{self_id};
    set<std::uint32_t>(s.game.current_stage, 4);
    set<std::int32_t>(s.game.stage_elapsed_sec, 200);
    set<std::int32_t>(s.game.stage_countdown_sec, 220);
    set<std::uint32_t>(s.game.economy, 800);
    set<std::uint32_t>(s.game.rune_status, 1);
    set<std::uint32_t>(s.game.red_base_hp, 10000);
    set<std::uint32_t>(s.game.red_base_max_hp, 10000);
    set<std::uint32_t>(s.game.blue_base_hp, 10000);
    set<std::uint32_t>(s.game.blue_base_max_hp, 10000);
    set<std::uint32_t>(s.game.red_base_status, 1);
    set<std::uint32_t>(s.game.blue_base_status, 1);
    set<std::uint32_t>(s.game.red_outpost_hp, 1000);
    set<std::uint32_t>(s.game.red_outpost_max_hp, 1000);
    set<std::uint32_t>(s.game.blue_outpost_hp, 1000);
    set<std::uint32_t>(s.game.blue_outpost_max_hp, 1000);
    set<std::uint32_t>(s.game.red_outpost_zone, 2);
    set<std::uint32_t>(s.game.blue_outpost_zone, 4);
    set<std::uint32_t>(s.game.red_base_zone, 2);
    set<std::uint32_t>(s.game.blue_base_zone, 4);
    set<std::uint32_t>(s.game.red_control_points, 60);
    set<std::uint32_t>(s.game.blue_control_points, 60);
    rm_terminal::RobotState& self = s.robots[RobotId{self_id}];
    set(self.respawn.pending, false);
    set<std::uint32_t>(self.dynamic.current_hp, 200);
    set<std::uint32_t>(self.statics.max_hp, 200);
    set<std::uint32_t>(self.statics.max_heat, 40);
    set(self.dynamic.remaining_ammo, ammo);
    rm_terminal::RobotHealth enemy;
    enemy.id = RobotId{103};
    enemy.faction = rm_terminal::kBlue;
    set<std::uint32_t>(enemy.current_hp, 20);
    set<std::uint32_t>(enemy.max_hp, 200);
    s.robot_health.push_back(enemy);
    rm_terminal::MapRobot seen;
    seen.id = RobotId{103};
    seen.faction = rm_terminal::kBlue;
    set(seen.position.x, 10.0);
    set(seen.position.y, 5.0);
    s.map_robots.push_back(seen);
    return s;
}

std::optional<NextCard> find(const Snapshot& s, NextRule rule) {
    for (const NextCard& card : rm_terminal::next_candidates(s, false))
        if (card.rule == rule) return card;
    return std::nullopt;
}

std::vector<NextCard> ranked(const Snapshot& s, std::size_t count = 3) {
    return rm_terminal::rank_next(rm_terminal::next_candidates(s, false), count);
}

// 1 打基地:弹够、4 轮热量内打得掉才算。
void attack_base_needs_ammo_and_heat() {
    Snapshot s = match_for(3, 20);
    set<std::uint32_t>(s.game.blue_base_hp, 300);
    auto card = find(s, NextRule::AttackBase);
    check(card && card->title == QStringLiteral("打敌方基地"), "300 hp base with 20 rounds is a kill");
    check(card && card->reason.contains(QStringLiteral("约 15 发")), "the card counts the rounds");

    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.remaining_ammo, 10);
    check(!find(s, NextRule::AttackBase), "10 rounds cannot kill a 300 hp base");
    check(find(s, NextRule::AttackBaseAfterAmmo).has_value(), "no ammo for the kill: buy ammo first");

    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.remaining_ammo, 0);
    card = find(s, NextRule::AttackBaseAfterAmmo);
    check(card && card->title == QStringLiteral("先补弹，补完打基地"), "0 rounds: buy ammo, then hit the base");

    set<std::uint32_t>(s.game.blue_base_hp, 10000);
    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.remaining_ammo, 20);
    check(find(s, NextRule::AttackBaseLongShot).has_value(), "a base that takes more than 4 heat cycles is a long shot");
    check(!find(s, NextRule::AttackBase), "and it is not the top-priority kill");

    set<std::uint32_t>(s.game.blue_base_status, 0);
    set<std::uint32_t>(s.game.blue_base_hp, 300);
    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.remaining_ammo, 20);
    check(!find(s, NextRule::AttackBase) && !find(s, NextRule::AttackBaseAfterAmmo) &&
              !find(s, NextRule::AttackBaseLongShot),
          "a base with 100% defense is never a target");

    s.robots[RobotId{3}].respawn.pending = rm_terminal::Field<bool>{};
    set(s.robots[RobotId{3}].respawn.pending, true);
    check(!find(s, NextRule::AttackBase) && !find(s, NextRule::AttackBaseAfterAmmo), "no attack cards while dead");
}

// 4 回防基地:有 100% 防御时掉血只可能是飞镖或判罚,不叫回防。
void defend_base_only_without_defense() {
    Snapshot s = match_for(3, 50);
    const auto with_hit = [&s](bool hit, std::uint32_t status) {
        set<std::uint32_t>(s.game.red_base_status, status);
        for (const NextCard& card : rm_terminal::next_candidates(s, hit))
            if (card.rule == NextRule::DefendBase) return true;
        return false;
    };
    check(with_hit(true, 1), "no defense + losing hp: go defend");
    check(!with_hit(true, 0), "100% defense + losing hp: no defend card");
    check(!with_hit(false, 1), "not losing hp: no defend card");
}

// 5 血少补血:25% 以下;远程只在最后 60 秒、金币够买弹又够回血。
void heal_threshold_and_remote() {
    Snapshot s = match_for(3, 50);
    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.current_hp, 48);   // 24%
    auto card = find(s, NextRule::Heal);
    check(card && card->title == QStringLiteral("补血"), "below 25%: heal");
    check(card && card->reason == QStringLiteral("本机只剩 24%"), "just the percentage, no method");

    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.current_hp, 50);   // 25%
    check(!find(s, NextRule::Heal), "25% is not below the line");

    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.current_hp, 40);
    set(s.robots[RobotId{3}].dynamic.can_remote_heal, true);
    check(find(s, NextRule::Heal)->title == QStringLiteral("补血"), "mid match: still plain heal");

    set<std::int32_t>(s.game.stage_countdown_sec, 30);
    check(find(s, NextRule::Heal)->title == QStringLiteral("远程回血"), "last 60 s + rich: remote heal");
    set<std::uint32_t>(s.game.economy, 100);
    check(find(s, NextRule::Heal)->title == QStringLiteral("补血"), "not enough gold to also buy ammo: plain heal");
}

// 6 补弹:只看弹数,卡片只写「补弹」。
void ammo_card_is_plain() {
    Snapshot s = match_for(3, 49);
    auto card = find(s, NextRule::BuyAmmo);
    check(card && card->title == QStringLiteral("补弹"), "49 rounds: buy ammo");
    check(card && card->reason == QStringLiteral("只剩 49 发"), "only the count");

    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.remaining_ammo, 50);
    check(!find(s, NextRule::BuyAmmo), "50 rounds is enough");
}

// 7/8 重装 42mm:先装,后打验证弹(规则 5.7.1)。
void heavy_installs_then_verifies() {
    Snapshot s = match_for(1, 0);
    rm_terminal::RobotState& self = s.robots[RobotId{1}];
    set<std::uint32_t>(self.statics.max_heat, 300);
    set<std::uint32_t>(s.tech_core.maximum_difficulty, 2);
    set<std::uint32_t>(s.tech_core.basic_state, 1);
    set<std::uint32_t>(s.game.tech_level, 1);
    set<std::uint32_t>(self.modules.modular_42mm, 0);
    check(!find(s, NextRule::BuyAmmo), "no ammo advice before the 42mm launcher works");
    check(!find(s, NextRule::FocusEnemy), "no focus fire while the heavy cannot shoot");
    check(!find(s, NextRule::OpenRune), "the heavy is never told to open the energy mechanism");
    check(!find(s, NextRule::InstallLauncher), "no workbench without the install permission");
    auto assemble = find(s, NextRule::Assemble);
    check(assemble && assemble->title == QStringLiteral("去装配 2 级"), "level 2 is the next first completion");

    set<std::uint32_t>(self.modules.modular_42mm, 1);
    check(find(s, NextRule::InstallLauncher).has_value(), "permission: go to the workbench");
    check(!find(s, NextRule::VerifyLauncher), "not mounted yet: no verify card");

    set<std::uint32_t>(self.modules.big_shooter, 1);
    check(!find(s, NextRule::InstallLauncher), "mounted: no more install card");
    auto verify = find(s, NextRule::VerifyLauncher);
    check(verify && verify->title == QStringLiteral("打一发验证弹"), "mounted but not verified: fire one verifying round");

    s.launcher_installed = true;
    check(!find(s, NextRule::VerifyLauncher), "installed: no verify card");
    check(find(s, NextRule::FocusEnemy).has_value(), "an armed heavy is told to focus fire");
    check(find(s, NextRule::BuyAmmo).has_value(), "and to buy 42mm when low");
}

// 9 打敌方前哨站。
void attack_the_enemy_outpost() {
    Snapshot s = match_for(3, 20);
    set<std::uint32_t>(s.game.blue_outpost_hp, 300);
    auto card = find(s, NextRule::AttackOutpost);
    check(card && card->reason.contains(QStringLiteral("约 15 发")), "300 hp outpost with 20 rounds");

    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.remaining_ammo, 5);
    check(!find(s, NextRule::AttackOutpost), "5 rounds cannot finish it");

    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.remaining_ammo, 20);
    set<std::uint32_t>(s.game.blue_outpost_hp, 0);
    check(!find(s, NextRule::AttackOutpost), "a destroyed outpost is not a target");
}

// 13/14 阵地:敌方临时控制要回防;自家前哨站倒了还站着,要写让开 5 秒。
void zone_cards() {
    Snapshot s = match_for(3, 50);
    set<std::uint32_t>(s.game.red_outpost_zone, 3);   // 蓝方临时控制红方前哨阵地
    check(find(s, NextRule::DefendZone).has_value(), "enemy temporary control: go defend");

    set<std::uint32_t>(s.game.red_outpost_zone, 1);   // 红方临时控制,前哨站还立着
    auto hold = find(s, NextRule::HoldZone);
    check(hold && hold->title == QStringLiteral("守住红方前哨区"), "holding a live outpost: keep holding");

    set<std::uint32_t>(s.game.red_outpost_hp, 0);     // 自家前哨站倒了
    hold = find(s, NextRule::HoldZone);
    check(hold && hold->title == QStringLiteral("让开 5 秒，等它重建再抢回来"),
          "our outpost is down: step off so it rebuilds, then take it back");

    set<std::uint32_t>(s.game.red_base_status, 0);    // 基地还有 100% 防御
    hold = find(s, NextRule::HoldZone);
    check(hold && hold->title == QStringLiteral("守住红方前哨区"), "base still defended: keeping the point is fine");
}

// 16 控制度落后去占区;17 最后 60 秒算输赢。
void score_and_endgame() {
    Snapshot s = match_for(3, 50);
    set<std::uint32_t>(s.game.red_control_points, 10);
    set<std::uint32_t>(s.game.blue_control_points, 30);
    set<std::uint32_t>(s.game.red_outpost_zone, 3);
    auto take = find(s, NextRule::TakeZone);
    check(take && take->title == QStringLiteral("去占红方前哨区"), "behind on control: take the home outpost zone");

    set<std::uint32_t>(s.game.red_control_points, 60);
    set<std::uint32_t>(s.game.blue_control_points, 90);
    take = find(s, NextRule::TakeZone);
    check(take && take->reason == QStringLiteral("控制度落后 30 分"), "the card counts the gap");

    check(!find(s, NextRule::Endgame), "not the last minute: no endgame card");
    set<std::uint32_t>(s.game.red_control_points, 90);
    set<std::uint32_t>(s.game.blue_control_points, 60);
    set<std::int32_t>(s.game.stage_countdown_sec, 30);
    auto end = find(s, NextRule::Endgame);
    check(end && end->title == QStringLiteral("现在结束就赢"), "ahead with 30 s left: winning if it ends now");

    set<std::uint32_t>(s.game.red_control_points, 60);
    set<std::uint32_t>(s.game.blue_control_points, 90);
    end = find(s, NextRule::Endgame);
    check(end && end->title == QStringLiteral("现在结束就输"), "behind: losing if it ends now");
}

// 先后:表的顺序就是枚举顺序,rank_next 取前几件。
void ranking_follows_the_table() {
    Snapshot s = match_for(3, 0);                       // 没弹:补弹 + 先补弹打基地
    set<std::uint32_t>(s.game.blue_base_hp, 300);
    set<std::uint32_t>(s.game.red_base_status, 1);
    const auto top = ranked(s, 3);
    check(!top.empty() && top.front().rule == NextRule::AttackBaseAfterAmmo, "buy-ammo-then-base comes first");
    check(top.size() >= 2 && top[1].rule == NextRule::BuyAmmo, "then the plain ammo card");
}

// 增益:我方攻击×2 发数减半,敌方建筑防御 50% 发数翻倍;虚弱时不叫人开枪,补弹照常。
void buffs_change_the_count() {
    Snapshot s = match_for(3, 20);
    set<std::uint32_t>(s.game.blue_base_hp, 300);
    rm_terminal::NextBuffs buffs;
    buffs.self_attack = 2.0;
    auto card = std::optional<NextCard>{};
    for (const NextCard& c : rm_terminal::next_candidates(s, false, buffs))
        if (c.rule == NextRule::AttackBase) card = c;
    check(card && card->reason.contains(QStringLiteral("约 8 发")), "attack x2: 300 hp takes 8 rounds");

    set<std::uint32_t>(s.robots[RobotId{3}].dynamic.remaining_ammo, 10);
    set<std::uint32_t>(s.game.blue_base_hp, 150);
    bool kill = false, after_ammo = false;
    for (const NextCard& c : rm_terminal::next_candidates(s, false, rm_terminal::NextBuffs{}))
        kill = kill || c.rule == NextRule::AttackBase;
    check(kill, "no defense: 150 hp takes 8 rounds, 10 are enough");
    buffs = rm_terminal::NextBuffs{};
    buffs.enemy_structure_defense = 50;
    kill = false;
    for (const NextCard& c : rm_terminal::next_candidates(s, false, buffs)) {
        kill = kill || c.rule == NextRule::AttackBase;
        after_ammo = after_ammo || c.rule == NextRule::AttackBaseAfterAmmo;
    }
    check(!kill && after_ammo, "enemy base 50% defense: 150 hp needs 15 rounds, 10 are not enough");

    s = match_for(3, 10);
    set<std::uint32_t>(s.game.blue_base_hp, 100);
    buffs = rm_terminal::NextBuffs{};
    buffs.self_weak = true;
    bool shooting = false, ammo = false;
    for (const NextCard& c : rm_terminal::next_candidates(s, false, buffs)) {
        shooting = shooting || c.rule == NextRule::AttackBase || c.rule == NextRule::FocusEnemy ||
                   c.rule == NextRule::OpenRune || c.rule == NextRule::AttackOutpost;
        ammo = ammo || c.rule == NextRule::BuyAmmo;
    }
    check(!shooting, "weak: the shooter is locked, no shooting cards");
    check(ammo, "weak: buying ammo still makes sense");
}

// 堡垒:我方基地护甲还没展开时,偷堡垒的那台 100% 易伤。
void fortress_thief_is_vulnerable() {
    Snapshot s = match_for(3, 50);
    set<std::int32_t>(s.game.red_fortress_activation_sec, 4);
    auto card = find(s, NextRule::DefendFortress);
    check(card && card->reason.contains(QStringLiteral("100% 易伤")), "the thief is 100% vulnerable");
    set<std::uint32_t>(s.game.red_base_status, 2);
    card = find(s, NextRule::DefendFortress);
    check(card && !card->reason.contains(QStringLiteral("易伤")), "armor already deployed: no vulnerability");

    // 雷达看得到站在我方堡垒增益点上的那台:写是谁,花名册那一行挂上卡片编号。
    set<std::uint32_t>(s.game.red_base_status, 1);
    set(s.map_robots[0].position.x, 6.6);
    set(s.map_robots[0].position.y, 7.5);
    card = find(s, NextRule::DefendFortress);
    check(card && card->reason.startsWith(QStringLiteral("敌方 3 号步兵在偷：4 秒 / 10 秒")) && card->enemy_id == 103u,
          "the robot on our fortress is named");
}

}  // namespace

int main() {
    attack_base_needs_ammo_and_heat();
    defend_base_only_without_defense();
    heal_threshold_and_remote();
    ammo_card_is_plain();
    heavy_installs_then_verifies();
    attack_the_enemy_outpost();
    zone_cards();
    score_and_endgame();
    ranking_follows_the_table();
    buffs_change_the_count();
    fortress_thief_is_vulnerable();
    if (failures != 0) {
        std::fprintf(stderr, "next_step_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("next_step_test: all checks passed\n");
    return 0;
}
