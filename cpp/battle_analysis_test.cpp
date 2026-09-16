#include "battle_analysis.h"

#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

template <class T>
Field<T> present(T value) {
    Field<T> field;
    field.value = value;
    field.quality = Quality::Valid;
    field.freshness = Freshness::Fresh;
    return field;
}

RobotHealth health(std::uint32_t id, std::uint32_t faction, std::uint32_t hp) {
    RobotHealth entry;
    entry.id = RobotId{id};
    entry.faction = faction;
    entry.current_hp = present<std::uint32_t>(hp);
    entry.max_hp = present<std::uint32_t>(600);
    return entry;
}

MapRobot self_marker(std::uint32_t id, std::uint32_t faction) {
    MapRobot robot;
    robot.id = RobotId{id};
    robot.faction = faction;
    robot.is_self = true;
    return robot;
}

const AnalysisMetric& by_label(const std::vector<AnalysisMetric>& metrics,
                               const QString& label) {
    for (const AnalysisMetric& metric : metrics)
        if (metric.label == label) return metric;
    throw std::runtime_error("metric not found");
}

Snapshot red_self_snapshot() {
    Snapshot snapshot;
    snapshot.map_robots.push_back(self_marker(1, 1));
    snapshot.game.red_economy = present<std::uint32_t>(900);
    snapshot.game.blue_economy = present<std::uint32_t>(750);
    snapshot.game.red_total_damage = present<std::uint32_t>(4000);
    snapshot.game.blue_total_damage = present<std::uint32_t>(5200);
    snapshot.game.red_fortress_sec = present<std::uint32_t>(42);
    snapshot.game.blue_fortress_sec = present<std::uint32_t>(42);
    snapshot.game.fortress_holder = present<std::uint32_t>(1);
    snapshot.robot_health.push_back(health(1, 1, 500));
    snapshot.robot_health.push_back(health(2, 1, 400));
    snapshot.robot_health.push_back(health(101, 2, 200));
    snapshot.robot_health.push_back(health(102, 2, 100));
    return snapshot;
}

void red_perspective_orients_self_to_red() {
    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(red_self_snapshot());
    const AnalysisMetric& economy = by_label(metrics, QStringLiteral("经济"));
    check(economy.self_value == 900 && economy.enemy_value == 750,
          "red self reads red economy as ours");
    check(metric_stance(economy) == MetricStance::Ahead, "higher economy reads as ahead");

    const AnalysisMetric& damage = by_label(metrics, QStringLiteral("总伤害"));
    check(damage.self_value == 4000 && damage.enemy_value == 5200,
          "red self reads red damage as ours");
    check(metric_stance(damage) == MetricStance::Behind, "lower damage reads as behind");
    check(*metric_diff(damage) == -1200, "damage diff is signed toward us");
}

// 蓝方视角必须整体镜像。本校每场可能被编在任一方,写死红方会把敌我完全倒置,
// 是这个面板最严重的失效模式,所以单独钉一条。
void blue_perspective_mirrors_every_metric() {
    Snapshot snapshot = red_self_snapshot();
    snapshot.map_robots.clear();
    snapshot.map_robots.push_back(self_marker(101, 2));

    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(snapshot);
    const AnalysisMetric& economy = by_label(metrics, QStringLiteral("经济"));
    check(economy.self_value == 750 && economy.enemy_value == 900,
          "blue self reads blue economy as ours");
    check(metric_stance(economy) == MetricStance::Behind, "mirrored economy reads as behind");

    check(fortress_hold(snapshot) == FortressHold::Theirs,
          "red-held fortress reads as theirs from blue seat");
}


// 缺失必须显示 "--" 而不是 0:0 伤害是「没打出去」,缺失是「服务器没给这个字段」,
// 把后者画成 0 会让操作手以为敌方毫无输出。
void absent_field_reads_as_placeholder_not_zero() {
    Snapshot snapshot = red_self_snapshot();
    snapshot.game.blue_total_damage = Field<std::uint32_t>{};

    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(snapshot);
    const AnalysisMetric& damage = by_label(metrics, QStringLiteral("总伤害"));
    check(!damage.enemy_value.has_value(), "absent damage stays nullopt");
    check(metric_value_text(damage.enemy_value) == QStringLiteral("--"),
          "absent damage renders as placeholder");
    check(!metric_diff(damage).has_value(), "diff against absent value is unknown");
    check(metric_stance(damage) == MetricStance::Unknown, "absent value has no stance");
    check(metric_stance_text(MetricStance::Unknown) == QStringLiteral("等待数据"),
          "unknown stance says waiting");
    check(metric_value_text(damage.self_value) == QStringLiteral("4000"),
          "present side still renders on a partially absent metric");
}

void real_zero_is_not_treated_as_missing() {
    Snapshot snapshot = red_self_snapshot();
    snapshot.game.blue_total_damage = present<std::uint32_t>(0);

    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(snapshot);
    const AnalysisMetric& damage = by_label(metrics, QStringLiteral("总伤害"));
    check(damage.enemy_value == 0, "a real zero survives as a value");
    check(metric_value_text(damage.enemy_value) == QStringLiteral("0"),
          "real zero renders as 0, not --");
    check(metric_stance(damage) == MetricStance::Ahead, "zero enemy damage means ahead");
}

void unknown_faction_yields_no_metrics_values() {
    Snapshot snapshot = red_self_snapshot();
    snapshot.map_robots.clear();

    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(snapshot);
    check(metrics.size() == 3, "metric list keeps its rows without a faction");
    for (const AnalysisMetric& metric : metrics) {
        check(!metric.self_value.has_value() && !metric.enemy_value.has_value(),
              "no faction means no oriented values");
        check(metric_stance(metric) == MetricStance::Unknown,
              "no faction means no stance");
    }
}

void equal_values_read_as_even() {
    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(red_self_snapshot());
    const AnalysisMetric& base = by_label(metrics, QStringLiteral("堡垒占领"));
    check(*metric_diff(base) == 0, "equal fortress seconds have zero diff");
    check(metric_stance(base) == MetricStance::Even, "equal values read as even");
    check(metric_stance_text(MetricStance::Even) == QStringLiteral("均势"),
          "even stance is distinct from waiting");
}


// 无人占领(协议给了 0)必须与没收到帧区分开:前者是可依赖的战场事实。
void fortress_hold_distinguishes_neutral_from_missing() {
    Snapshot snapshot = red_self_snapshot();
    check(fortress_hold(snapshot) == FortressHold::Ours, "red holder from red seat is ours");

    snapshot.game.fortress_holder = present<std::uint32_t>(0);
    check(fortress_hold(snapshot) == FortressHold::Neutral, "holder 0 is nobody");
    check(fortress_hold_text(FortressHold::Neutral) == QStringLiteral("无人占领"),
          "neutral says nobody holds it");

    snapshot.game.fortress_holder = Field<std::uint32_t>{};
    check(fortress_hold(snapshot) == FortressHold::Unknown, "absent holder is unknown");
    check(fortress_hold_text(FortressHold::Unknown) == QStringLiteral("等待数据"),
          "unknown holder says waiting");

    Snapshot no_seat = red_self_snapshot();
    no_seat.map_robots.clear();
    check(fortress_hold(no_seat) == FortressHold::Unknown,
          "without a faction the holder cannot be oriented");
}

}  // namespace

int main() {
    try {
        red_perspective_orients_self_to_red();
        blue_perspective_mirrors_every_metric();
        absent_field_reads_as_placeholder_not_zero();
        real_zero_is_not_treated_as_missing();
        unknown_faction_yields_no_metrics_values();
        equal_values_read_as_even();
        fortress_hold_distinguishes_neutral_from_missing();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "battle analysis metrics OK\n";
    return 0;
}
