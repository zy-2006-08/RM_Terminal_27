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

const AnalysisMetric& by_label(const std::vector<AnalysisMetric>& metrics,
                               const QString& label) {
    for (const AnalysisMetric& metric : metrics)
        if (metric.label == label) return metric;
    throw std::runtime_error("metric not found");
}

Snapshot red_self_snapshot() {
    Snapshot snapshot;
    snapshot.self = RobotId{1};
    snapshot.game.current_stage = present<std::uint32_t>(4);
    snapshot.game.economy = present<std::uint32_t>(900);
    snapshot.game.total_economy = present<std::uint64_t>(2400);
    snapshot.game.red_total_damage = present<std::uint32_t>(4000);
    snapshot.game.blue_total_damage = present<std::uint32_t>(5200);
    snapshot.game.rune_status = present<std::uint32_t>(2);
    snapshot.game.rune_activated_arms = present<std::uint32_t>(3);
    snapshot.kills.red = 2;
    snapshot.kills.blue = 2;
    return snapshot;
}

void red_perspective_orients_self_to_red() {
    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(red_self_snapshot());
    check(metrics.size() == 4, "four metric cards");
    const AnalysisMetric& economy = by_label(metrics, QStringLiteral("我方经济"));
    check(economy.self_value == 900 && !economy.enemy_value, "economy is ours only");
    check(!economy.has_enemy && economy.note == QStringLiteral("累计 2400"),
          "economy shows the running total instead of an enemy value");
    check(metric_stance(economy) == MetricStance::Unknown, "one-sided metric has no stance");

    const AnalysisMetric& damage = by_label(metrics, QStringLiteral("总伤害"));
    check(damage.self_value == 4000 && damage.enemy_value == 5200,
          "red self reads red damage as ours");
    check(metric_stance(damage) == MetricStance::Behind, "lower damage reads as behind");
    check(*metric_diff(damage) == -1200, "damage diff is signed toward us");

    const AnalysisMetric& kills = by_label(metrics, QStringLiteral("击杀"));
    check(kills.self_value == 2 && kills.enemy_value == 2, "kills from the local tally");
    check(metric_stance(kills) == MetricStance::Even, "equal kills read as even");
    check(metric_stance_text(MetricStance::Even) == QStringLiteral("均势"),
          "even stance is distinct from waiting");

    const AnalysisMetric& rune = by_label(metrics, QStringLiteral("能量机关"));
    check(rune.text_value == QStringLiteral("激活中") && rune.note == QStringLiteral("3 臂"),
          "rune shows its state and arms");
    check(!rune.has_enemy, "rune status is ours only");
    check(rune.icon == QStringLiteral(":/images/message/validate_icon_rune.png"),
          "energy metric carries the rune icon");
}

// 蓝方视角必须整体镜像。本校每场可能被编在任一方,写死红方会把敌我完全倒置,
// 是这个面板最严重的失效模式,所以单独钉一条。
void blue_perspective_mirrors_every_metric() {
    Snapshot snapshot = red_self_snapshot();
    snapshot.self = RobotId{101};
    snapshot.kills.red = 3;

    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(snapshot);
    const AnalysisMetric& damage = by_label(metrics, QStringLiteral("总伤害"));
    check(damage.self_value == 5200 && damage.enemy_value == 4000,
          "blue self reads blue damage as ours");
    check(metric_stance(damage) == MetricStance::Ahead, "mirrored damage reads as ahead");
    const AnalysisMetric& kills = by_label(metrics, QStringLiteral("击杀"));
    check(kills.self_value == 2 && kills.enemy_value == 3, "blue self reads blue kills as ours");

    snapshot.game.red_fortress_activation_sec = present<std::int32_t>(6);
    snapshot.game.blue_fortress_activation_sec = present<std::int32_t>(0);
    const FortressStatus fortress = fortress_status(snapshot);
    check(fortress.theirs_by_us == 6 && fortress.ours_by_enemy == 0,
          "blue seat: activating the red fortress is our progress");
    check(fortress_tone(fortress) == FortressTone::Good, "our activation reads as good");
}

// 缺失必须显示 "--" 而不是 0:0 伤害是「没打出去」,缺失是「服务器没给这个字段」。
void absent_field_reads_as_placeholder_not_zero() {
    Snapshot snapshot = red_self_snapshot();
    snapshot.game.blue_total_damage = Field<std::uint32_t>{};

    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(snapshot);
    const AnalysisMetric& damage = by_label(metrics, QStringLiteral("总伤害"));
    check(!damage.enemy_value.has_value(), "absent damage stays nullopt");
    check(metric_value_text(damage.enemy_value) == QStringLiteral("--"),
          "absent damage renders as placeholder");
    check(metric_stance(damage) == MetricStance::Unknown, "absent value has no stance");
    check(metric_stance_text(MetricStance::Unknown) == QStringLiteral("等待数据"),
          "unknown stance says waiting");

    snapshot.game.current_stage = Field<std::uint32_t>{};
    const AnalysisMetric& kills = by_label(build_analysis_metrics(snapshot), QStringLiteral("击杀"));
    check(!kills.self_value && !kills.enemy_value, "no match data: kills are not 0 yet");
}

void real_zero_is_not_treated_as_missing() {
    Snapshot snapshot = red_self_snapshot();
    snapshot.game.blue_total_damage = present<std::uint32_t>(0);
    const AnalysisMetric& damage =
        by_label(build_analysis_metrics(snapshot), QStringLiteral("总伤害"));
    check(damage.enemy_value == 0, "a real zero survives as a value");
    check(metric_value_text(damage.enemy_value) == QStringLiteral("0"),
          "real zero renders as 0, not --");
    check(metric_stance(damage) == MetricStance::Ahead, "zero enemy damage means ahead");
}

void unknown_seat_yields_no_metric_values() {
    Snapshot snapshot = red_self_snapshot();
    snapshot.self.reset();
    const std::vector<AnalysisMetric> metrics = build_analysis_metrics(snapshot);
    check(metrics.size() == 4, "metric list keeps its rows without a seat");
    for (const AnalysisMetric& metric : metrics) {
        check(!metric.self_value.has_value() && !metric.enemy_value.has_value() &&
                  metric.text_value.isEmpty(),
              "no seat means no oriented values");
    }
    check(fortress_tone(fortress_status(snapshot)) == FortressTone::Unknown,
          "without a seat the fortress cannot be oriented");
}

// 堡垒:没人在激活(协议给了 0)和没收到这一帧要分开。
void fortress_distinguishes_quiet_from_missing() {
    Snapshot snapshot = red_self_snapshot();
    check(fortress_status_text(fortress_status(snapshot)) == QStringLiteral("等待数据"),
          "no mechanism frame yet");
    snapshot.game.red_fortress_activation_sec = present<std::int32_t>(0);
    snapshot.game.blue_fortress_activation_sec = present<std::int32_t>(0);
    check(fortress_tone(fortress_status(snapshot)) == FortressTone::Quiet, "zero timers are quiet");
    check(fortress_status_text(fortress_status(snapshot)) == QStringLiteral("双方堡垒都没人激活"),
          "quiet text");
    snapshot.game.red_fortress_activation_sec = present<std::int32_t>(7);
    const FortressStatus status = fortress_status(snapshot);
    check(fortress_tone(status) == FortressTone::Danger, "enemy on our fortress is danger");
    check(fortress_status_text(status).contains(QStringLiteral("敌方在激活我方堡垒 7s/10s")),
          "danger text names the progress");
}

// 阵地编码 0..4 是解码层换算后的绝对编码,映射错一个就会把敌方阵地画成我方的。
void zone_codes_map_to_four_slots() {
    Snapshot snapshot;
    snapshot.game.red_outpost_zone = present<std::uint32_t>(2);
    snapshot.game.blue_outpost_zone = present<std::uint32_t>(0);
    snapshot.game.red_base_zone = present<std::uint32_t>(3);
    snapshot.game.blue_base_zone = present<std::uint32_t>(4);
    snapshot.game.red_base_zone_progress = present<std::int32_t>(-4);

    const ZoneBoard board = zone_board(snapshot);
    check(board.zones[0].name == QStringLiteral("红方前哨区"), "slot 0 is the red outpost");
    check(board.zones[1].name == QStringLiteral("蓝方前哨区"), "slot 1 is the blue outpost");
    check(board.zones[2].name == QStringLiteral("红方基地区"), "slot 2 is the red base");
    check(board.zones[3].name == QStringLiteral("蓝方基地区"), "slot 3 is the blue base");
    check(board.zones[0].control == ZoneControl::RedFull, "code 2 is red full control");
    check(board.zones[1].control == ZoneControl::Neutral, "code 0 is uncontrolled");
    check(board.zones[2].control == ZoneControl::BlueTemporary, "code 3 is blue temporary");
    check(board.zones[3].control == ZoneControl::BlueFull, "code 4 is blue full control");
    check(zone_status_text(board.zones[2]) == QStringLiteral("红方基地区 蓝方临时（蓝方推进 4s）"),
          "a pushing zone shows who is pushing");
    check(zone_status_text(board.zones[0]) == QStringLiteral("红方前哨区 红方完全"),
          "a quiet zone shows only its control");
    check(zone_status_brief_text(board.zones[2]) == QStringLiteral("红基地 蓝临时 蓝推4s"),
          "the brief form keeps who controls and who pushes");
    check(zone_status_brief_text(board.zones[1]) == QStringLiteral("蓝前哨 未控"),
          "the brief form of an uncontrolled zone");
}

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

void global_line_marks_missing_values() {
    Snapshot snapshot;
    snapshot.game.red_control_points = present<std::uint32_t>(12);
    const ZoneBoard board = zone_board(snapshot);
    check(board.red_control_points == 12u, "control points come from the server");
    check(!board.blue_control_points.has_value(), "absent control points stay nullopt");
    const QString text = zone_global_text(board);
    check(text.contains(QStringLiteral("红 12")), "present control points render");
    check(text.contains(QStringLiteral("--")), "absent values render as placeholder");
    check(!text.contains(QStringLiteral("加固")), "2027 has no reinforce chances");
}

}  // namespace

int main() {
    try {
        red_perspective_orients_self_to_red();
        blue_perspective_mirrors_every_metric();
        absent_field_reads_as_placeholder_not_zero();
        real_zero_is_not_treated_as_missing();
        unknown_seat_yields_no_metric_values();
        fortress_distinguishes_quiet_from_missing();
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
