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
    // nullopt = 缺数据,调用方显示 "--"。缺失和真实的 0 是两件事:0 是「没打出伤害」,
    // 缺失是「服务器没给」。
    std::optional<std::int64_t> self_value;
    std::optional<std::int64_t> enemy_value;
    QString unit;
    // qrc 路径,空 = 该指标不配图标,标签从卡片左边距起排。
    QString icon;
    // 协议只给我方的指标(经济、能量机关):不画「敌 --」和优劣条,右侧改写 note。
    bool has_enemy = true;
    QString note;
    // 非空时代替 self_value 显示(能量机关是状态,不是数)。
    QString text_value;
};

// 我方视角:经济(只有我方)、总伤害、击杀(Event 1 本机累计)、能量机关(只有我方)。
// 阵营取自开机选定的席位;没有席位时双方数值一律为空,不默认按红方算。
std::vector<AnalysisMetric> build_analysis_metrics(const Snapshot& snapshot);

// 堡垒(2027):双方基地区各有一个堡垒增益点,对方激活满 10 秒,这一方的基地护甲展开。
struct FortressStatus {
    std::optional<std::int32_t> ours_by_enemy;  // 我方堡垒正被敌方激活了几秒,0 = 没人在激活
    std::optional<std::int32_t> theirs_by_us;   // 敌方堡垒正被我方激活了几秒
};
enum class FortressTone { Unknown, Danger, Good, Quiet };

FortressStatus fortress_status(const Snapshot& snapshot);
QString fortress_status_text(const FortressStatus& status);
FortressTone fortress_tone(const FortressStatus& status);

std::optional<std::int64_t> metric_diff(const AnalysisMetric& metric);
MetricStance metric_stance(const AnalysisMetric& metric);
QString metric_value_text(std::optional<std::int64_t> value);
QString metric_stance_text(MetricStance stance);

// 4 个阵地(前哨/基地 × 红/蓝)各自的控制状态。
// Unknown 是「没收到这一帧」,与真实的未控(协议明确给了 0)必须分开。
enum class ZoneControl { Unknown, Neutral, RedTemporary, RedFull, BlueTemporary, BlueFull };

struct ZoneStatus {
    QString name;
    ZoneControl control = ZoneControl::Unknown;
    // 激活计时:>0 红方在推进,<0 蓝方在推进,0 或缺失不显示。
    std::optional<std::int32_t> progress;
};

// 全局区:双方控制度与后勤系数。后勤系数由基地剩余血量推出(规则手册 5.6.1,
// λ = 1 - 已损失血量 / 20000)。
struct ZoneBoard {
    std::array<ZoneStatus, 4> zones{};
    std::optional<std::uint32_t> red_control_points, blue_control_points;
    std::optional<double> red_logistics, blue_logistics;
};

ZoneBoard zone_board(const Snapshot& snapshot);
QString zone_control_text(ZoneControl control);
QString zone_status_text(const ZoneStatus& zone);
// 窄格子用的短名:「红前哨 蓝临时」「蓝基地 红完全 红推4s」。
QString zone_status_brief_text(const ZoneStatus& zone);
QString zone_global_text(const ZoneBoard& board);

}
