#include "battle_analysis.h"

#include "roster_pane.h"

#include <QString>

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
                       QStringLiteral("金")});
    metrics.push_back({QStringLiteral("总伤害"),
                       pick(g.red_total_damage, g.blue_total_damage, true),
                       pick(g.red_total_damage, g.blue_total_damage, false),
                       QString()});
    metrics.push_back({QStringLiteral("堡垒占领"),
                       pick(g.red_fortress_sec, g.blue_fortress_sec, true),
                       pick(g.red_fortress_sec, g.blue_fortress_sec, false),
                       QStringLiteral("s")});
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

}  // namespace rm_terminal
