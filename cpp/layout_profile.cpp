#include "layout_profile.h"

namespace rm_terminal {

const LayoutProfile& layout_profile(Role role) {
    // 步兵:三列。左列上 我方花名册、下 本车指令;中间地图、右列敌方花名册都跨这两行;
    // 下排 事件 / 战场数据分析 / 图传。本车指令放在我方一侧,和花名册下方原来的空白合用。
    static const LayoutProfile infantry{
        Role::Infantry,
        {0, 5, 0},
        {5, 2, 3},
        {0, 2},
        -1,
        0,
        {{PaneId::AllyRoster, 0, 0}, {PaneId::SelfCommands, 1, 0}, {PaneId::Map, 0, 1, 2, 1},
         {PaneId::EnemyRoster, 0, 2, 2, 1},
         {PaneId::Events, 2, 0}, {PaneId::Analysis, 2, 1}, {PaneId::Video, 2, 2}}};
    // 重装:地图横跨中间两列,左列同步兵;下排四格 事件 / 战场数据分析 / 装配 / 图传。
    static const LayoutProfile hero{
        Role::Hero,
        {0, 3, 3, 0},
        {5, 2, 3},
        {0, 3},
        -1,
        0,
        {{PaneId::AllyRoster, 0, 0}, {PaneId::SelfCommands, 1, 0}, {PaneId::Map, 0, 1, 2, 2},
         {PaneId::EnemyRoster, 0, 3, 2, 1},
         {PaneId::Events, 2, 0}, {PaneId::Analysis, 2, 1}, {PaneId::Assembly, 2, 2},
         {PaneId::Video, 2, 3}}};
    // 指挥手:左列上下两块花名册,地图占两行,右列 飞镖 / 哨兵;下排 事件 / 战场数据分析 / 图传。
    static const LayoutProfile commander{
        Role::Commander,
        {0, 5, 0},
        {3, 3, 2},
        {0},
        2,
        380,
        {{PaneId::AllyRoster, 0, 0}, {PaneId::EnemyRoster, 1, 0}, {PaneId::Map, 0, 1, 2, 1},
         {PaneId::Dart, 0, 2}, {PaneId::Sentry, 1, 2},
         {PaneId::Events, 2, 0}, {PaneId::Analysis, 2, 1}, {PaneId::Video, 2, 2}}};
    switch (role) {
    case Role::Hero: return hero;
    case Role::Commander: return commander;
    case Role::Infantry: break;
    }
    return infantry;
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
    }
    return QString();
}

}  // namespace rm_terminal
