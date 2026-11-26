#pragma once

#include "seat.h"

#include <QRect>
#include <QSize>
#include <QString>
#include <optional>
#include <utility>
#include <vector>

namespace rm_terminal
{

    // 主界面信息页里能放进格子的面板。SelfStatus / Timeline 不在默认布局里,是组件库里能加的。
    // BigControl 已经不用了(改成战场数据分析的「只看控制度」),SelfCommands(「本车」)也删掉了
    // (功能都改成官方选手端的按键),留着名字是为了读旧文件时认得它们。
    enum class PaneId
    {
        AllyRoster,
        EnemyRoster,
        Map,
        Events,
        Analysis,
        Video,
        Assembly,
        Dart,
        Sentry,
        SelfCommands,
        NextStep,
        BigControl,
        SelfStatus,
        Timeline,
    };

    // 信息页按格子排:16 列 × 9 行(效果图 9:地图是「8 × 5 格」)。位置按格子记,不按像素记,
    // 换显示器、换分辨率都照样能用。
    constexpr int kGridColumns = 16;
    constexpr int kGridRows = 9;

    struct PaneSlot
    {
        PaneId pane;
        int row = 0;
        int column = 0;
        int row_span = 1;
        int column_span = 1;

        QRect units() const { return QRect(column, row, column_span, row_span); }
        bool operator==(const PaneSlot& other) const
        {
            return pane == other.pane && row == other.row && column == other.column && row_span == other.row_span &&
                   column_span == other.column_span;
        }
    };

    // 一种席位的布局:每个面板放在哪几格。三种布局共用状态条、顶栏、提醒、弹窗和图传全屏页,
    // 只在信息页的格子里换面板。
    struct LayoutProfile
    {
        Role role = Role::Infantry;
        std::vector<PaneSlot> cells; // 不叫 slots:那是 Qt 的宏
        // 不占格子、装在别的面板里的面板:{面板, 宿主}。步兵和重装的图传默认并进「本车」面板
        // 下半截;界面设置里把「图传」单独拖成一块,它就从「本车」里搬出来。
        std::vector<std::pair<PaneId, PaneId>> embedded;
    };

    const LayoutProfile& layout_profile(Role role);
    // 面板装在哪个面板里;占自己格子的面板返回空。
    std::optional<PaneId> embedded_host(const LayoutProfile& profile, PaneId pane);
    // 布局 JSON 和测试里用的英文名。
    QString pane_name(PaneId pane);

    // 这个席位能放进画面的所有组件:默认布局里的,加上组件库里「还可以加」的。
    std::vector<PaneId> available_panes(Role role);
    // 界面设置里能缩到多小,以及从组件库拖进来时默认多大(格)。
    QSize pane_min_units(PaneId pane);
    QSize pane_default_units(PaneId pane);

} // namespace rm_terminal
