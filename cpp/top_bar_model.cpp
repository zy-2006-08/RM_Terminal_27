#include "top_bar_model.h"

#include "presentation.h"

namespace rm_terminal {

namespace {

QString stage_label(std::uint32_t stage) {
    switch (stage) {
    case 0: return QStringLiteral("未开始");
    case 1: return QStringLiteral("准备");
    case 2: return QStringLiteral("自检");
    case 3: return QStringLiteral("倒计时");
    case 4: return QStringLiteral("比赛中");
    case 5: return QStringLiteral("已结束");
    default: return QStringLiteral("未知");
    }
}

QString clock_of(const Field<std::int32_t>& field) {
    if (!field.value) return QStringLiteral("--:--");
    const int total = *field.value < 0 ? 0 : *field.value;
    return QStringLiteral("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString number_of(const Field<std::uint32_t>& field) {
    return field.value ? QString::number(*field.value) : QStringLiteral("--");
}

void seed_slots(TopBarSide& side, std::uint32_t id_base) {
    for (std::size_t i = 0; i < kTopBarSlotsPerSide; ++i) {
        const std::uint32_t id = id_base + kFieldRobots[i];
        side.cards[i].id = id;
        side.cards[i].display_number = display_robot_number(id);
        side.cards[i].robot_class = robot_class_name(id);
    }
}

}  // namespace

TopBarModel top_bar_model(const Snapshot& snapshot) {
    const auto& game = snapshot.game;
    TopBarModel model;

    seed_slots(model.red, 0);
    seed_slots(model.blue, 100);

    model.red.base_hp = game.red_base_hp.value;
    model.red.base_max_hp = game.red_base_max_hp.value;
    model.blue.base_hp = game.blue_base_hp.value;
    model.blue.base_max_hp = game.blue_base_max_hp.value;

    model.red.outpost_hp = game.red_outpost_hp.value;
    model.red.outpost_max_hp = game.red_outpost_max_hp.value;
    model.blue.outpost_hp = game.blue_outpost_hp.value;
    model.blue.outpost_max_hp = game.blue_outpost_max_hp.value;

    auto outpost_blue = [](const Field<std::uint32_t>& zone, bool fallback) {
        if (zone.quality != Quality::Valid || !zone.value) return fallback;
        return *zone.value == 3u || *zone.value == 4u;
    };
    model.red.outpost_controlled_by_blue = outpost_blue(game.red_outpost_zone, false);
    model.blue.outpost_controlled_by_blue = outpost_blue(game.blue_outpost_zone, true);

    const auto valid = [](const Field<std::uint32_t>& field) -> std::optional<std::uint32_t> {
        return field.quality == Quality::Valid ? field.value : std::nullopt;
    };
    model.red.base_status = valid(game.red_base_status);
    model.blue.base_status = valid(game.blue_base_status);
    model.red.control_points = valid(game.red_control_points);
    model.blue.control_points = valid(game.blue_control_points);
    // 2027 协议不发队名:team_name 留空,QML 回落到不显示,不编造校名。

    model.red.score = number_of(game.red_score);
    model.blue.score = number_of(game.blue_score);

    // 全场血量来自 GlobalUnitStatus。不能用 snapshot.robots(RobotDynamicStatus),那条
    // 链路只承载自机,拿它填槽位会让另外 7 台永远是灰的。
    for (const RobotHealth& robot : snapshot.robot_health) {
        TopBarSide* side = nullptr;
        if (robot.faction == kRed) side = &model.red;
        else if (robot.faction == kBlue) side = &model.blue;
        else continue;

        for (TopBarSlot& slot : side->cards) {
            if (slot.id != robot.id.value) continue;
            slot.online = true;
            slot.hp = robot.current_hp.value;
            slot.max_hp = robot.max_hp.value;
        }
    }

    model.clock = clock_of(game.stage_countdown_sec);
    model.stage = game.current_stage.value ? stage_label(*game.current_stage.value)
                                           : QStringLiteral("--");
    model.round = (game.current_round.value && game.total_rounds.value)
                      ? QStringLiteral("%1/%2")
                            .arg(*game.current_round.value)
                            .arg(*game.total_rounds.value)
                      : QStringLiteral("-/-");
    const auto remaining = game.stage_countdown_sec.value;
    model.clock_critical = remaining.has_value() && *remaining >= 0 && *remaining <= 10;

    return model;
}

}  // namespace rm_terminal
