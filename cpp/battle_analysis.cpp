#include "battle_analysis.h"

#include "roster_pane.h"

#include <QString>

#include <algorithm>

namespace rm_terminal {

namespace {

std::optional<std::int64_t> as_int(const Field<std::uint32_t>& field) {
    if (field.quality != Quality::Valid || !field.value) return std::nullopt;
    return static_cast<std::int64_t>(*field.value);
}

}  // namespace

std::vector<AnalysisMetric> build_analysis_metrics(const Snapshot& snapshot) {
    const std::optional<std::uint32_t> self = self_faction(snapshot);
    const bool red = self && *self == 1u;
    const bool known = self.has_value();

    const GameFields<Field>& g = snapshot.game;
    auto pick = [known, red](const Field<std::uint32_t>& red_field,
                            const Field<std::uint32_t>& blue_field,
                            bool want_self) -> std::optional<std::int64_t> {
        if (!known) return std::nullopt;
        return as_int(red == want_self ? red_field : blue_field);
    };

    std::vector<AnalysisMetric> metrics;
    metrics.push_back({QStringLiteral("经济"),
                       pick(g.red_economy, g.blue_economy, true),
                       pick(g.red_economy, g.blue_economy, false),
                       QStringLiteral("金"),
                       QString()});
    metrics.push_back({QStringLiteral("总伤害"),
                       pick(g.red_total_damage, g.blue_total_damage, true),
                       pick(g.red_total_damage, g.blue_total_damage, false),
                       QString(),
                       QString()});
    metrics.push_back({QStringLiteral("堡垒占领"),
                       pick(g.red_fortress_sec, g.blue_fortress_sec, true),
                       pick(g.red_fortress_sec, g.blue_fortress_sec, false),
                       QStringLiteral("s"),
                       QString()});
    metrics.push_back({QStringLiteral("能量机关"),
                       pick(g.red_energy_activations, g.blue_energy_activations, true),
                       pick(g.red_energy_activations, g.blue_energy_activations, false),
                       QStringLiteral("次"),
                       QStringLiteral(":/images/message/validate_icon_rune.png")});
    return metrics;
}

FortressHold fortress_hold(const Snapshot& snapshot) {
    const std::optional<std::uint32_t> self = self_faction(snapshot);
    const Field<std::uint32_t>& holder = snapshot.game.fortress_holder;
    if (!self || holder.quality != Quality::Valid || !holder.value)
        return FortressHold::Unknown;
    if (*holder.value == 0) return FortressHold::Neutral;
    return *holder.value == *self ? FortressHold::Ours : FortressHold::Theirs;
}

QString fortress_hold_text(FortressHold hold) {
    switch (hold) {
        case FortressHold::Ours: return QStringLiteral("我方占领");
        case FortressHold::Theirs: return QStringLiteral("敌方占领");
        case FortressHold::Neutral: return QStringLiteral("无人占领");
        case FortressHold::Unknown: break;
    }
    return QStringLiteral("等待数据");
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
    board.zones = {{{QStringLiteral("红方前哨区"), ZoneControl::Unknown},
                    {QStringLiteral("蓝方前哨区"), ZoneControl::Unknown},
                    {QStringLiteral("红方基地区"), ZoneControl::Unknown},
                    {QStringLiteral("蓝方基地区"), ZoneControl::Unknown}}};

    const Field<std::uint32_t>* raw[4] = {&g.red_outpost_zone, &g.blue_outpost_zone,
                                          &g.red_base_zone, &g.blue_base_zone};
    for (std::size_t i = 0; i < 4; ++i) {
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

    auto point = [](const Field<std::uint32_t>& f)
        -> std::optional<std::uint32_t> {
        return f.quality == Quality::Valid ? f.value : std::nullopt;
    };
    board.red_control_points = point(g.red_control_points);
    board.blue_control_points = point(g.blue_control_points);
    board.red_reinforce = point(g.red_reinforce_chances);
    board.blue_reinforce = point(g.blue_reinforce_chances);

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

QString zone_global_text(const ZoneBoard& board) {
    const QString unknown = QStringLiteral("--");
    auto number = [&unknown](const std::optional<std::uint32_t>& v) {
        return v ? QString::number(*v) : unknown;
    };
    auto ratio = [&unknown](const std::optional<double>& v) {
        return v ? QString::number(*v, 'f', 2) : unknown;
    };
    return QStringLiteral("控制度 红 %1 / 蓝 %2 ｜ 加固 红 %3 / 蓝 %4 ｜ 后勤 红 %5 / 蓝 %6")
        .arg(number(board.red_control_points), number(board.blue_control_points),
             number(board.red_reinforce), number(board.blue_reinforce),
             ratio(board.red_logistics), ratio(board.blue_logistics));
}

}  // namespace rm_terminal
