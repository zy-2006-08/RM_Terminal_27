#include "layout_profile.h"

#include <algorithm>

namespace rm_terminal {

namespace {

PaneSlot at(PaneId pane, int column, int row, int width, int height) { return {pane, row, column, height, width}; }

}  // namespace

const LayoutProfile& layout_profile(Role role) {
    // 步兵(效果图 v5):上排 我方机器人 / 战场数据分析 / 敌方机器人,下排 本车 + 图传 /
    // 战术地图 / 战场事件。步兵最关心的控制度和阵地放在正中间,地图缩小放到下面。
    // 本车和图传各是一块(本车下面就是图传),不再把图传塞进本车里。
    // 「下一步」三张卡在中间一列最上面,不开的方案里它那两格由战场数据分析接过去。
    static const LayoutProfile infantry{
        Role::Infantry,
        {at(PaneId::AllyRoster, 0, 0, 4, 4), at(PaneId::NextStep, 4, 0, 8, 2), at(PaneId::Analysis, 4, 2, 8, 2),
         at(PaneId::EnemyRoster, 12, 0, 4, 4), at(PaneId::SelfCommands, 0, 4, 4, 3), at(PaneId::Video, 0, 7, 4, 2),
         at(PaneId::Map, 4, 4, 8, 5), at(PaneId::Events, 12, 4, 4, 5)},
        {}};
    // 重装:和步兵一样,右下角多一块装配,战场事件挪到装配下面。
    static const LayoutProfile hero{
        Role::Hero,
        {at(PaneId::AllyRoster, 0, 0, 4, 4), at(PaneId::NextStep, 4, 0, 8, 2), at(PaneId::Analysis, 4, 2, 8, 2),
         at(PaneId::EnemyRoster, 12, 0, 4, 4), at(PaneId::SelfCommands, 0, 4, 4, 3), at(PaneId::Video, 0, 7, 4, 2),
         at(PaneId::Map, 4, 4, 8, 5), at(PaneId::Assembly, 12, 4, 4, 3), at(PaneId::Events, 12, 7, 4, 2)},
        {}};
    // 指挥手:左列 我方 / 敌方花名册 / 战场事件,中间地图(带标记工具条)和战场数据分析,
    // 右列 飞镖 / 哨兵 / 图传。哨兵按钮最多,给它四格高。指挥手看全局,地图保持最大。
    static const LayoutProfile commander{
        Role::Commander,
        {at(PaneId::AllyRoster, 0, 0, 4, 3), at(PaneId::EnemyRoster, 0, 3, 4, 3), at(PaneId::Events, 0, 6, 4, 3),
         at(PaneId::Map, 4, 0, 8, 6), at(PaneId::Analysis, 4, 6, 8, 3), at(PaneId::Dart, 12, 0, 4, 3),
         at(PaneId::Sentry, 12, 3, 4, 4), at(PaneId::Video, 12, 7, 4, 2)},
        {}};
    switch (role) {
    case Role::Hero: return hero;
    case Role::Commander: return commander;
    case Role::Infantry: break;
    }
    return infantry;
}

std::optional<PaneId> embedded_host(const LayoutProfile& profile, PaneId pane) {
    for (const auto& [inner, host] : profile.embedded)
        if (inner == pane) return host;
    return std::nullopt;
}

QString pane_name(PaneId pane) {
    switch (pane) {
    case PaneId::AllyRoster: return QStringLiteral("ally_roster");
    case PaneId::EnemyRoster: return QStringLiteral("enemy_roster");
    case PaneId::Map: return QStringLiteral("map");
    case PaneId::Events: return QStringLiteral("events");
    case PaneId::Analysis: return QStringLiteral("analysis");
    case PaneId::Video: return QStringLiteral("video");
    case PaneId::Assembly: return QStringLiteral("assembly");
    case PaneId::Dart: return QStringLiteral("dart");
    case PaneId::Sentry: return QStringLiteral("sentry");
    case PaneId::SelfCommands: return QStringLiteral("self_commands");
    case PaneId::NextStep: return QStringLiteral("next_step");
    case PaneId::BigControl: return QStringLiteral("big_control");
    case PaneId::SelfStatus: return QStringLiteral("self_status");
    case PaneId::Timeline: return QStringLiteral("timeline");
    }
    return QString();
}

std::vector<PaneId> available_panes(Role role) {
    std::vector<PaneId> out;
    for (const PaneSlot& slot : layout_profile(role).cells) out.push_back(slot.pane);
    for (const auto& [inner, host] : layout_profile(role).embedded) out.push_back(inner);
    for (const PaneId extra : {PaneId::SelfStatus, PaneId::Timeline})
        if (std::find(out.begin(), out.end(), extra) == out.end()) out.push_back(extra);
    return out;
}

QSize pane_min_units(PaneId pane) {
    switch (pane) {
    case PaneId::Map: return {4, 3};
    case PaneId::SelfCommands: return {3, 3};
    case PaneId::Analysis:
    case PaneId::NextStep: return {4, 2};
    case PaneId::Video: return {2, 2};
    case PaneId::Events:
    case PaneId::BigControl:
    case PaneId::SelfStatus:
    case PaneId::Timeline: return {3, 2};
    case PaneId::AllyRoster:
    case PaneId::EnemyRoster:
    case PaneId::Assembly:
    case PaneId::Dart:
    case PaneId::Sentry: return {3, 3};
    }
    return {3, 2};
}

QSize pane_default_units(PaneId pane) {
    switch (pane) {
    case PaneId::Events: return {4, 5};
    case PaneId::Analysis: return {8, 4};
    case PaneId::NextStep: return {8, 2};
    case PaneId::Video:
    case PaneId::SelfStatus:
    case PaneId::Timeline: return {4, 3};
    case PaneId::BigControl: return {4, 2};
    case PaneId::Map: return {8, 5};
    default: break;
    }
    return pane_min_units(pane);
}

}  // namespace rm_terminal
