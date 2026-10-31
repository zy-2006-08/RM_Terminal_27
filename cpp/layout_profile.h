#pragma once

#include "seat.h"

#include <QString>
#include <vector>

namespace rm_terminal {

// 主界面信息页里能放进格子的面板。
enum class PaneId { AllyRoster, EnemyRoster, Map, Events, Analysis, Video, Assembly, Dart, Sentry, SelfCommands };

struct PaneSlot {
    PaneId pane;
    int row = 0;
    int column = 0;
    int row_span = 1;
    int column_span = 1;
};

// 一种席位的布局:网格每列/每行的拉伸比例,哪几列钉宽,每个面板放在哪一格。
// 三种布局共用状态条、顶栏、提醒、弹窗和图传全屏页,只在信息页的格子里换面板。
struct LayoutProfile {
    Role role = Role::Infantry;
    std::vector<int> column_stretch;
    std::vector<int> row_stretch;
    // 钉成 kRosterColumnWidth 的列(花名册列),两侧等宽。
    std::vector<int> roster_columns;
    // 指挥手右列(飞镖、哨兵)的最小宽度,0 = 不钉。
    int side_column = -1;
    int side_column_width = 0;
    std::vector<PaneSlot> cells;  // 不叫 slots:那是 Qt 的宏
};

const LayoutProfile& layout_profile(Role role);
// 布局 JSON 和测试里用的英文名。
QString pane_name(PaneId pane);

}  // namespace rm_terminal
