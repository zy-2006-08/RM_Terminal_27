#pragma once

#include "domain.h"
#include <QString>
#include <set>

namespace rm_terminal {

QString freshness_text(Freshness freshness);
QString quality_text(Quality quality);
QString snapshot_text(const Snapshot& snapshot);

// Authoritative source: core/constants.py:80-86 CHASSIS_MODES. Lives here rather
// than in dashboard.cpp so CTest can pin the values; drift between the Python and
// C++ names is a wrong readout on the operator's screen, not a cosmetic issue.
QString chassis_name(std::uint32_t mode);

// RMUC 2027 阵容（规则手册表 2-1）：每方 5 台 = 重装 1、步兵 2、空中 1、哨兵 1，
// 连续编号 1 重装 / 2,3 步兵 / 4 空中 / 5 哨兵。
// 正式协议尚未发布，以下编号→兵种的对应为本地模拟词汇，不得视为正式合同；
// 规则正式发布后此表是唯一需要修改的地方。
// 蓝方内部沿用 +100 偏移（附录二既有约定），故 101 与 1 同为重装。
// 返回空字符串表示该编号不在已知阵容内 —— 调用方必须显示编号本身而非编造兵种。
QString robot_class_name(std::uint32_t robot_id);

// 官方选手端两侧均显示 1-5、以红/蓝配色区分阵营，不显示 101+ 的内部偏移编号。
// 传入内部 ID，返回操作手看到的编号。
std::uint32_t display_robot_number(std::uint32_t robot_id);

// Reports fields that aged into Stale since the previous call, so a field that
// stays stale logs once per transition rather than once per snapshot.
class StaleReporter {
public:
    void inspect(const Snapshot& snapshot);
private:
    std::set<QString> stale_;
};

class PresentationState {
public:
    explicit PresentationState(Snapshot snapshot) : snapshot_(std::move(snapshot)) {}
    const Snapshot& snapshot() const { return snapshot_; }
private:
    const Snapshot snapshot_;
};

}
