#pragma once

#include "domain.h"

#include <QString>
#include <array>
#include <optional>
#include <vector>

namespace rm_terminal {

// 指标的优劣方向。Unknown 与 Even 必须分开:Unknown 是没有数据,Even 是数据齐全
// 且双方相等,前者不能显示成「均势」。
enum class MetricStance { Unknown, Ahead, Even, Behind };

struct AnalysisMetric {
    QString label;
    // nullopt = 该指标缺少至少一侧数据,调用方显示 "--"。协议里这四个字段都是
    // optional,缺失和真实的 0 是两件事:0 是「没打出伤害」,缺失是「服务器没给」。
    std::optional<std::int64_t> self_value;
    std::optional<std::int64_t> enemy_value;
    QString unit;
    // qrc 路径,空 = 该指标不配图标,标签从卡片左边距起排。
    QString icon;
};

// 我方视角。self_faction() 为空(尚未收到任何 is_self 标记)时返回的指标全部为
// nullopt,而不是默认按红方算 —— 本校每场可能被编在红方或蓝方,猜错会把敌我倒置。
std::vector<AnalysisMetric> build_analysis_metrics(const Snapshot& snapshot);

// 堡垒当前归属。Neutral(协议给了 0,确实无人占领)与 Unknown(没收到这一帧)
// 必须分开:前者是可依赖的战场事实,后者不是。
enum class FortressHold { Unknown, Ours, Theirs, Neutral };

FortressHold fortress_hold(const Snapshot& snapshot);
QString fortress_hold_text(FortressHold hold);

std::optional<std::int64_t> metric_diff(const AnalysisMetric& metric);
MetricStance metric_stance(const AnalysisMetric& metric);
QString metric_value_text(std::optional<std::int64_t> value);
QString metric_stance_text(MetricStance stance);

// 2027 规则：4 个阵地（前哨/基地 × 红/蓝）各自的占领进度。
// Unknown 是「没收到这一帧」，与真实的未控（协议明确给了 0）必须分开。
enum class ZoneControl { Unknown, Neutral, RedTemporary, RedFull, BlueTemporary, BlueFull };

struct ZoneStatus {
    QString name;
    ZoneControl control = ZoneControl::Unknown;
};

// 全局区：双方控制度、加固机会与后勤系数。控制度和加固由服务器累计下发，
// 后勤系数由基地剩余血量占比推出（λ = 1 - 已损失/最大血量）。
struct ZoneBoard {
    std::array<ZoneStatus, 4> zones{};
    std::optional<std::uint32_t> red_control_points, blue_control_points;
    std::optional<std::uint32_t> red_reinforce, blue_reinforce;
    std::optional<double> red_logistics, blue_logistics;
};

ZoneBoard zone_board(const Snapshot& snapshot);
QString zone_control_text(ZoneControl control);
QString zone_global_text(const ZoneBoard& board);

}
