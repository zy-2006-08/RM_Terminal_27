#include "battle_analysis.h"

#include "roster_pane.h"

#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdlib>

namespace rm_terminal {

namespace {

template<class T>
std::optional<std::int64_t> as_int(const Field<T>& field) {
    if (field.quality != Quality::Valid || !field.value) return std::nullopt;
    return static_cast<std::int64_t>(*field.value);
}

QString rune_text(const GameFields<Field>& g) {
    if (g.rune_status.quality != Quality::Valid || !g.rune_status.value) return QString();
    switch (*g.rune_status.value) {
        case 1: return QStringLiteral("未激活");
        case 2: return QStringLiteral("激活中");
        case 3: return QStringLiteral("已激活");
        default: return QStringLiteral("--");
    }
}

}  // namespace

std::vector<AnalysisMetric> build_analysis_metrics(const Snapshot& snapshot) {
    const std::optional<std::uint32_t> self = self_faction(snapshot);
    const bool known = self.has_value();
    const bool red = known && *self == kRed;
    const GameFields<Field>& g = snapshot.game;
    auto pick = [known, red](std::optional<std::int64_t> red_value,
                             std::optional<std::int64_t> blue_value,
                             bool want_self) -> std::optional<std::int64_t> {
        if (!known) return std::nullopt;
        return red == want_self ? red_value : blue_value;
    };

    std::vector<AnalysisMetric> metrics;

    AnalysisMetric economy{QStringLiteral("我方经济"), known ? as_int(g.economy) : std::nullopt,
                           std::nullopt, QStringLiteral("金"), QString()};
    economy.has_enemy = false;
    if (const auto total = as_int(g.total_economy))
        economy.note = QStringLiteral("累计 %1").arg(*total);
    metrics.push_back(economy);

    metrics.push_back({QStringLiteral("总伤害"),
                       pick(as_int(g.red_total_damage), as_int(g.blue_total_damage), true),
                       pick(as_int(g.red_total_damage), as_int(g.blue_total_damage), false),
                       QString(), QString()});

    // 击杀在本机累计:终端收到过比赛状态才算在数,否则 0 只是「还没开始数」。
    const bool counting = g.current_stage.quality == Quality::Valid;
    const auto red_kills = counting ? std::optional<std::int64_t>(snapshot.kills.red) : std::nullopt;
    const auto blue_kills = counting ? std::optional<std::int64_t>(snapshot.kills.blue) : std::nullopt;
    metrics.push_back({QStringLiteral("击杀"), pick(red_kills, blue_kills, true),
                       pick(red_kills, blue_kills, false), QString(), QString()});

    AnalysisMetric rune{QStringLiteral("能量机关"), std::nullopt, std::nullopt, QString(),
                        QStringLiteral(":/images/message/validate_icon_rune.png")};
    rune.has_enemy = false;
    if (known) {
        rune.text_value = rune_text(g);
        if (const auto arms = as_int(g.rune_activated_arms); arms && *arms > 0)
            rune.note = QStringLiteral("%1 臂").arg(*arms);
    }
    metrics.push_back(rune);
    return metrics;
}

FortressStatus fortress_status(const Snapshot& snapshot) {
    FortressStatus status;
    const std::optional<std::uint32_t> self = self_faction(snapshot);
    if (!self) return status;
    const auto value = [](const Field<std::int32_t>& field) -> std::optional<std::int32_t> {
        return field.quality == Quality::Valid ? field.value : std::nullopt;
    };
    const bool red = *self == kRed;
    status.ours_by_enemy = value(red ? snapshot.game.red_fortress_activation_sec
                                     : snapshot.game.blue_fortress_activation_sec);
    status.theirs_by_us = value(red ? snapshot.game.blue_fortress_activation_sec
                                    : snapshot.game.red_fortress_activation_sec);
    return status;
}

QString fortress_status_text(const FortressStatus& status) {
    if (!status.ours_by_enemy && !status.theirs_by_us) return QStringLiteral("等待数据");
    QStringList parts;
    if (status.ours_by_enemy && *status.ours_by_enemy > 0)
        parts << QStringLiteral("敌方在激活我方堡垒 %1s/10s").arg(*status.ours_by_enemy);
    if (status.theirs_by_us && *status.theirs_by_us > 0)
        parts << QStringLiteral("我方在激活敌方堡垒 %1s/10s").arg(*status.theirs_by_us);
    if (parts.isEmpty()) return QStringLiteral("双方堡垒都没人激活");
    return parts.join(QStringLiteral(" ｜ "));
}

FortressTone fortress_tone(const FortressStatus& status) {
    if (!status.ours_by_enemy && !status.theirs_by_us) return FortressTone::Unknown;
    if (status.ours_by_enemy && *status.ours_by_enemy > 0) return FortressTone::Danger;
    if (status.theirs_by_us && *status.theirs_by_us > 0) return FortressTone::Good;
    return FortressTone::Quiet;
}

std::optional<std::int64_t> metric_diff(const AnalysisMetric& metric) {
    if (!metric.self_value || !metric.enemy_value) return std::nullopt;
    return *metric.self_value - *metric.enemy_value;
}

MetricStance metric_stance(const AnalysisMetric& metric) {
    const std::optional<std::int64_t> diff = metric_diff(metric);
    if (!diff) return MetricStance::Unknown;
    if (*diff > 0) return MetricStance::Ahead;
    if (*diff < 0) return MetricStance::Behind;
    return MetricStance::Even;
}

QString metric_value_text(std::optional<std::int64_t> value) {
    return value ? QString::number(*value) : QStringLiteral("--");
}

QString metric_stance_text(MetricStance stance) {
    switch (stance) {
        case MetricStance::Ahead: return QStringLiteral("我方领先");
        case MetricStance::Behind: return QStringLiteral("敌方领先");
        case MetricStance::Even: return QStringLiteral("均势");
        case MetricStance::Unknown: break;
    }
    return QStringLiteral("等待数据");
}

ZoneBoard zone_board(const Snapshot& snapshot) {
    const GameFields<Field>& g = snapshot.game;
    ZoneBoard board;
    board.zones = {{{QStringLiteral("红方前哨区"), ZoneControl::Unknown, std::nullopt},
                    {QStringLiteral("蓝方前哨区"), ZoneControl::Unknown, std::nullopt},
                    {QStringLiteral("红方基地区"), ZoneControl::Unknown, std::nullopt},
                    {QStringLiteral("蓝方基地区"), ZoneControl::Unknown, std::nullopt}}};

    const Field<std::uint32_t>* raw[4] = {&g.red_outpost_zone, &g.blue_outpost_zone,
                                          &g.red_base_zone, &g.blue_base_zone};
    const Field<std::int32_t>* progress[4] = {&g.red_outpost_zone_progress,
                                              &g.blue_outpost_zone_progress,
                                              &g.red_base_zone_progress, &g.blue_base_zone_progress};
    for (std::size_t i = 0; i < 4; ++i) {
        if (progress[i]->quality == Quality::Valid) board.zones[i].progress = progress[i]->value;
        if (raw[i]->quality != Quality::Valid || !raw[i]->value) continue;
        switch (*raw[i]->value) {
            case 0: board.zones[i].control = ZoneControl::Neutral; break;
            case 1: board.zones[i].control = ZoneControl::RedTemporary; break;
            case 2: board.zones[i].control = ZoneControl::RedFull; break;
            case 3: board.zones[i].control = ZoneControl::BlueTemporary; break;
            case 4: board.zones[i].control = ZoneControl::BlueFull; break;
            default: break;
        }
    }

    auto point = [](const Field<std::uint32_t>& f) -> std::optional<std::uint32_t> {
        return f.quality == Quality::Valid ? f.value : std::nullopt;
    };
    board.red_control_points = point(g.red_control_points);
    board.blue_control_points = point(g.blue_control_points);

    auto logistics = [](const Field<std::uint32_t>& hp,
                        const Field<std::uint32_t>& max_hp) -> std::optional<double> {
        if (hp.quality != Quality::Valid || !hp.value) return std::nullopt;
        if (max_hp.quality != Quality::Valid || !max_hp.value || *max_hp.value == 0)
            return std::nullopt;
        // λ = 1 - (基地损失血量) / 20000,并限制在规则规定的 [0.5, 1.0]。
        const double lost_hp = static_cast<double>(*max_hp.value) - *hp.value;
        return std::clamp(1.0 - lost_hp / 20000.0, 0.5, 1.0);
    };
    board.red_logistics = logistics(g.red_base_hp, g.red_base_max_hp);
    board.blue_logistics = logistics(g.blue_base_hp, g.blue_base_max_hp);
    return board;
}

QString zone_control_text(ZoneControl control) {
    switch (control) {
        case ZoneControl::Neutral: return QStringLiteral("未控");
        case ZoneControl::RedTemporary: return QStringLiteral("红方临时");
        case ZoneControl::RedFull: return QStringLiteral("红方完全");
        case ZoneControl::BlueTemporary: return QStringLiteral("蓝方临时");
        case ZoneControl::BlueFull: return QStringLiteral("蓝方完全");
        case ZoneControl::Unknown: break;
    }
    return QStringLiteral("--");
}

QString zone_status_text(const ZoneStatus& zone) {
    QString text = QStringLiteral("%1 %2").arg(zone.name, zone_control_text(zone.control));
    if (zone.progress && *zone.progress != 0)
        text += QStringLiteral("（%1推进 %2s）")
                    .arg(*zone.progress > 0 ? QStringLiteral("红方") : QStringLiteral("蓝方"))
                    .arg(std::abs(*zone.progress));
    return text;
}

QString zone_status_brief_text(const ZoneStatus& zone) {
    QString name = zone.name;
    name.remove(QStringLiteral("方")).remove(QStringLiteral("区"));
    QString control = zone_control_text(zone.control);
    control.remove(QStringLiteral("方"));
    QString text = QStringLiteral("%1 %2").arg(name, control);
    if (zone.progress && *zone.progress != 0)
        text += QStringLiteral(" %1推%2s")
                    .arg(*zone.progress > 0 ? QStringLiteral("红") : QStringLiteral("蓝"))
                    .arg(std::abs(*zone.progress));
    return text;
}

QString zone_global_text(const ZoneBoard& board) {
    const QString unknown = QStringLiteral("--");
    auto number = [&unknown](const std::optional<std::uint32_t>& v) {
        return v ? QString::number(*v) : unknown;
    };
    auto ratio = [&unknown](const std::optional<double>& v) {
        return v ? QString::number(*v, 'f', 2) : unknown;
    };
    return QStringLiteral("控制度 红 %1 / 蓝 %2 ｜ 后勤 红 %3 / 蓝 %4")
        .arg(number(board.red_control_points), number(board.blue_control_points),
             ratio(board.red_logistics), ratio(board.blue_logistics));
}

}  // namespace rm_terminal
