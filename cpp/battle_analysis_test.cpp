#include "battle_analysis.h"

#include <cmath>
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
    snapshot.game.red_energy_activations = present<std::uint32_t>(3);
    snapshot.game.blue_energy_activations = present<std::uint32_t>(1);
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

    const AnalysisMetric& rune = by_label(metrics, QStringLiteral("能量机关"));
    check(rune.self_value == 3 && rune.enemy_value == 1,
          "red self reads red energy activations as ours");
    check(metric_stance(rune) == MetricStance::Ahead, "more activations reads as ahead");
    check(rune.icon == QStringLiteral(":/images/message/validate_icon_rune.png"),
          "energy metric carries the rune icon");
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

    const AnalysisMetric& rune = by_label(metrics, QStringLiteral("能量机关"));
    check(rune.self_value == 1 && rune.enemy_value == 3,
          "blue self reads blue energy activations as ours");
    check(metric_stance(rune) == MetricStance::Behind,
          "mirrored activations read as behind");

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
    check(metrics.size() == 4, "metric list keeps its rows without a faction");
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

// 阵地编码 0..4 是服务器契约,映射错一个就会把敌方基地画成我方的。四阵地顺序
// 同时钉住:红前哨、蓝前哨、红基地、蓝基地。
void zone_codes_map_to_four_slots() {
    Snapshot snapshot;
    snapshot.game.red_outpost_zone = present<std::uint32_t>(2);
    snapshot.game.blue_outpost_zone = present<std::uint32_t>(0);
    snapshot.game.red_base_zone = present<std::uint32_t>(3);
    snapshot.game.blue_base_zone = present<std::uint32_t>(4);

    const ZoneBoard board = zone_board(snapshot);
    check(board.zones[0].name == QStringLiteral("红方前哨区"), "slot 0 is the red outpost");
    check(board.zones[1].name == QStringLiteral("蓝方前哨区"), "slot 1 is the blue outpost");
    check(board.zones[2].name == QStringLiteral("红方基地区"), "slot 2 is the red base");
    check(board.zones[3].name == QStringLiteral("蓝方基地区"), "slot 3 is the blue base");
    check(board.zones[0].control == ZoneControl::RedFull, "code 2 is red full control");
    check(board.zones[1].control == ZoneControl::Neutral, "code 0 is uncontrolled");
    check(board.zones[2].control == ZoneControl::BlueTemporary, "code 3 is blue temporary");
    check(board.zones[3].control == ZoneControl::BlueFull, "code 4 is blue full control");
    check(zone_control_text(ZoneControl::RedTemporary) == QStringLiteral("红方临时"),
          "temporary control has its own text");
}

// 未控(协议给 0)和没收到这一帧是两件事,和堡垒归属同样的区分。
void absent_zone_stays_unknown() {
    Snapshot snapshot;
    snapshot.game.red_outpost_zone = present<std::uint32_t>(0);
    const ZoneBoard board = zone_board(snapshot);
    check(board.zones[0].control == ZoneControl::Neutral, "present zero is neutral");
    check(board.zones[1].control == ZoneControl::Unknown, "absent zone is unknown");
    check(zone_control_text(ZoneControl::Unknown) == QStringLiteral("--"),
          "unknown zone renders as placeholder, not uncontrolled");
}

// 后勤系数 λ = 1 - 基地损失血量 / 20000,满血为 1、基地摧毁时为 0.5。
void logistics_is_half_at_zero_base() {
    Snapshot snapshot;
    snapshot.game.red_base_hp = present<std::uint32_t>(5000);
    snapshot.game.red_base_max_hp = present<std::uint32_t>(10000);
    snapshot.game.blue_base_hp = present<std::uint32_t>(10000);
    snapshot.game.blue_base_max_hp = present<std::uint32_t>(10000);
    ZoneBoard board = zone_board(snapshot);
    check(board.red_logistics && std::abs(*board.red_logistics - 0.75) < 1e-9,
           "half-damaged red base yields 0.75 logistics");
    check(board.blue_logistics && std::abs(*board.blue_logistics - 1.0) < 1e-9,
           "full blue base yields 1.0 logistics");

    snapshot.game.red_base_hp = present<std::uint32_t>(0);
    board = zone_board(snapshot);
    check(board.red_logistics && std::abs(*board.red_logistics - 0.5) < 1e-9,
          "destroyed red base yields 0.5 logistics");

    const Snapshot empty;
    board = zone_board(empty);
    check(!board.red_logistics.has_value(), "missing base hp yields no logistics");
}

// 全局区一行里三类数据各自缺失时都显示 --,不能把缺失画成 0。
void global_line_marks_missing_values() {
    Snapshot snapshot;
    snapshot.game.red_control_points = present<std::uint32_t>(12);
    snapshot.game.blue_reinforce_chances = present<std::uint32_t>(2);
    const ZoneBoard board = zone_board(snapshot);
    check(board.red_control_points == 12, "control points come from the server");
    check(!board.blue_control_points.has_value(), "absent control points stay nullopt");
    const QString text = zone_global_text(board);
    check(text.contains(QStringLiteral("红 12")), "present control points render");
    check(text.contains(QStringLiteral("--")), "absent values render as placeholder");
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
        zone_codes_map_to_four_slots();
        absent_zone_stays_unknown();
        logistics_is_half_at_zero_base();
        global_line_marks_missing_values();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "battle analysis metrics OK\n";
    return 0;
}
