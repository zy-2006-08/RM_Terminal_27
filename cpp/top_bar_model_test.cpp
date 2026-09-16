#include "top_bar_model.h"

#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

template <class T>
Field<T> present(T value, Freshness freshness = Freshness::Fresh) {
    Field<T> field;
    field.value = value;
    field.quality = Quality::Valid;
    field.freshness = freshness;
    return field;
}

RobotHealth health(std::uint32_t id, std::uint32_t faction, std::uint32_t hp,
                   std::uint32_t max_hp) {
    RobotHealth entry;
    entry.id = RobotId{id};
    entry.faction = faction;
    entry.current_hp = present<std::uint32_t>(hp);
    entry.max_hp = present<std::uint32_t>(max_hp);
    return entry;
}

Snapshot full_field_snapshot() {
    Snapshot snapshot;
    snapshot.game.red_base_hp = present<std::uint32_t>(1456);
    snapshot.game.red_base_max_hp = present<std::uint32_t>(1500);
    snapshot.game.blue_base_hp = present<std::uint32_t>(1417);
    snapshot.game.blue_base_max_hp = present<std::uint32_t>(1500);
    snapshot.game.red_score = present<std::uint32_t>(1);
    snapshot.game.blue_score = present<std::uint32_t>(2);
    snapshot.game.stage_countdown_sec = present<std::int32_t>(273);
    snapshot.game.current_stage = present<std::uint32_t>(4);
    snapshot.game.current_round = present<std::uint32_t>(1);
    snapshot.game.total_rounds = present<std::uint32_t>(3);

    for (std::uint32_t n = 1; n <= 5; ++n) {
        snapshot.robot_health.push_back(health(n, 1, 300 + n * 40, 600));
        snapshot.robot_health.push_back(health(100 + n, 2, 500 - n * 30, 600));
    }
    return snapshot;
}

// 用户明确要求红方 4/5 号不得再是灰色。灰色的成因是这两台没有血量来源,所以这里
// 断言的是「协议报了就在线且带血量」,而不是断言颜色。
void every_reported_robot_fills_its_slot() {
    const TopBarModel model = top_bar_model(full_field_snapshot());

    for (std::size_t i = 0; i < kTopBarSlotsPerSide; ++i) {
        check(model.red.cards[i].online, "every reported red robot is online");
        check(model.red.cards[i].has_hp(), "every reported red robot carries HP");
        check(model.blue.cards[i].online, "every reported blue robot is online");
        check(model.blue.cards[i].has_hp(), "every reported blue robot carries HP");
    }

    check(model.red.cards[3].display_number == 4, "red slot 4 shows as number 4");
    check(model.red.cards[3].robot_class == QStringLiteral("空中"), "red 4 is the air robot");
    check(model.red.cards[4].robot_class == QStringLiteral("哨兵"), "red 5 is the sentry");
}

// 蓝方 101-105 的内部偏移不得泄漏到界面上,官方两侧都显示 1-5。
void blue_ids_are_displayed_without_the_internal_offset() {
    const TopBarModel model = top_bar_model(full_field_snapshot());
    for (std::size_t i = 0; i < kTopBarSlotsPerSide; ++i) {
        check(model.blue.cards[i].id == 101 + static_cast<std::uint32_t>(i),
              "the blue slot keeps its internal id");
        check(model.blue.cards[i].display_number == static_cast<std::uint32_t>(i) + 1,
              "the blue slot displays 1-5 rather than 101-105");
    }
}

// 缺席必须是占位,不能补成在线/满血 —— 否则界面谎报一台不存在的机器人。
void an_absent_robot_is_a_placeholder_not_a_healthy_one() {
    Snapshot snapshot = full_field_snapshot();
    std::vector<RobotHealth> kept;
    for (const RobotHealth& entry : snapshot.robot_health) {
        if (entry.id.value != 4) kept.push_back(entry);
    }
    snapshot.robot_health = kept;

    const TopBarModel model = top_bar_model(snapshot);
    check(!model.red.cards[3].online, "the absent robot is not reported online");
    check(!model.red.cards[3].has_hp(), "the absent robot carries no HP");
    check(model.red.cards[3].display_number == 4, "the absent slot still holds its place");
    check(model.red.cards[3].robot_class == QStringLiteral("空中"),
          "the absent slot still names its class so the position is identifiable");
    check(model.red.cards[2].online, "its neighbours are unaffected");
}

// 基地血量必须来自 GameStatus 的基地字段,而不是全队血量合计。
void base_hp_comes_from_the_base_fields() {
    const TopBarModel model = top_bar_model(full_field_snapshot());
    check(model.red.has_base_hp() && *model.red.base_hp == 1456, "red base HP is the base field");
    check(model.blue.has_base_hp() && *model.blue.base_hp == 1417,
          "blue base HP is present rather than missing");
    check(*model.red.base_max_hp == 1500 && *model.blue.base_max_hp == 1500,
          "both bases carry their maximum");
}

// 没有基地血量时不得画成满血,必须报告为缺失。
void a_missing_base_hp_is_reported_as_missing() {
    Snapshot snapshot = full_field_snapshot();
    snapshot.game.blue_base_hp = Field<std::uint32_t>{};
    const TopBarModel model = top_bar_model(snapshot);
    check(!model.blue.has_base_hp(), "a missing base HP is not silently filled in");
    check(model.red.has_base_hp(), "the other side is unaffected");
}

void score_and_clock_read_from_the_protocol() {
    const TopBarModel model = top_bar_model(full_field_snapshot());
    check(model.red.score == QStringLiteral("1") && model.blue.score == QStringLiteral("2"),
          "the score is the round score, not a cumulative points figure");
    check(model.clock == QStringLiteral("04:33"), "the clock is mm:ss of the countdown");
    check(model.round == QStringLiteral("1/3"), "the round reads current/total");
    check(model.stage == QStringLiteral("比赛中"), "the stage is named");
    check(!model.clock_critical, "a mid-match clock is not critical");
}

void an_empty_snapshot_states_that_nothing_is_known() {
    const Snapshot empty;
    const TopBarModel model = top_bar_model(empty);

    check(model.clock == QStringLiteral("--:--"), "an absent countdown reads as unknown");
    check(model.round == QStringLiteral("-/-"), "an absent round reads as unknown");
    check(model.red.score == QStringLiteral("--"), "an absent score reads as unknown");
    check(!model.red.has_base_hp() && !model.blue.has_base_hp(),
          "absent base HP is not invented");

    for (std::size_t i = 0; i < kTopBarSlotsPerSide; ++i) {
        check(!model.red.cards[i].online && !model.red.cards[i].has_hp(),
              "no robot is claimed online without data");
        check(model.red.cards[i].display_number == static_cast<std::uint32_t>(i) + 1,
              "the layout still holds 5 cards per side");
    }
}

// 最后 10 秒转红。边界值单独钉住:11 秒不该报警,0 秒必须报警。
void the_clock_turns_critical_in_the_last_ten_seconds() {
    Snapshot snapshot = full_field_snapshot();

    snapshot.game.stage_countdown_sec = present<std::int32_t>(11);
    check(!top_bar_model(snapshot).clock_critical, "11 seconds is not yet critical");

    snapshot.game.stage_countdown_sec = present<std::int32_t>(10);
    check(top_bar_model(snapshot).clock_critical, "10 seconds is critical");

    snapshot.game.stage_countdown_sec = present<std::int32_t>(0);
    const TopBarModel expired = top_bar_model(snapshot);
    check(expired.clock_critical, "0 seconds is critical");
    check(expired.clock == QStringLiteral("00:00"), "an expired clock reads 00:00");
}

// 0 血是「阵亡」,和「没有数据」是两件事,不能都塌成灰色占位。
void a_dead_robot_is_distinct_from_an_absent_one() {
    Snapshot snapshot = full_field_snapshot();
    snapshot.robot_health[0] = health(1, 1, 0, 600);

    const TopBarModel model = top_bar_model(snapshot);
    check(model.red.cards[0].online, "a dead robot is still online");
    check(model.red.cards[0].has_hp(), "a dead robot still carries HP data");
    check(*model.red.cards[0].hp == 0, "a dead robot reports zero HP");
}

// 未知编号不得挤掉正常槽位,也不得凭空多出一格。
void an_out_of_roster_id_is_ignored() {
    Snapshot snapshot = full_field_snapshot();
    snapshot.robot_health.push_back(health(7, 1, 500, 600));
    snapshot.robot_health.push_back(health(107, 2, 500, 600));

    const TopBarModel model = top_bar_model(snapshot);
    check(model.red.cards.size() == kTopBarSlotsPerSide, "the red side keeps exactly 5 cards");
    for (std::size_t i = 0; i < kTopBarSlotsPerSide; ++i) {
        check(model.red.cards[i].display_number == static_cast<std::uint32_t>(i) + 1,
              "an unknown id does not displace a real slot");
    }
}

}  // namespace

int main() {
    try {
        every_reported_robot_fills_its_slot();
        blue_ids_are_displayed_without_the_internal_offset();
        an_absent_robot_is_a_placeholder_not_a_healthy_one();
        base_hp_comes_from_the_base_fields();
        a_missing_base_hp_is_reported_as_missing();
        score_and_clock_read_from_the_protocol();
        an_empty_snapshot_states_that_nothing_is_known();
        the_clock_turns_critical_in_the_last_ten_seconds();
        a_dead_robot_is_distinct_from_an_absent_one();
        an_out_of_roster_id_is_ignored();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "top_bar_model: all checks passed\n";
    return 0;
}
