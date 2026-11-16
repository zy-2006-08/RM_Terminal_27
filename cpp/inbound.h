#pragma once

#include "domain.h"

namespace rm_terminal::inbound {

// 解码层交给 Store 的补丁。protobuf 只在 decoder.cpp 里出现;这里已经换算成红蓝、
// 米和本地编号,字段缺失用 std::nullopt 表示,和真实的 0 分开。
using GameStatus = GameFields<std::optional>;
using RobotDynamicStatus = DynamicFields<std::optional>;
using RobotStaticStatus = StaticFields<std::optional>;
using RobotModuleStatus = ModuleFields<std::optional>;
using RobotPosition = PositionFields<std::optional>;
using RobotRespawnStatus = RespawnFields<std::optional>;
using RobotBuff = BuffFields<std::optional>;
using RobotInjury = InjuryFields<std::optional>;
using TechCoreStatus = TechCoreFields<std::optional>;
using DartStatus = DartFields<std::optional>;
using SentryStatus = SentryFields<std::optional>;

struct RobotHealthEntry {
    std::uint32_t robot_id = 0;
    std::optional<std::uint32_t> current_hp;
    std::optional<std::int32_t> bullets;
};
// GlobalUnitStatus.robot_health / robot_bullets 按编号展开后的结果。
struct RobotHealthSet {
    std::vector<RobotHealthEntry> entries;
};

struct RadarEntry {
    std::uint32_t robot_id = 0;
    std::optional<double> x, y;  // 米;协议给 0,0 时视为没有位置
    std::uint32_t highlight = 0;
};
struct RadarSet {
    std::vector<RadarEntry> entries;
};

struct Event {
    std::int32_t event_id = 0;
    std::string param;
};

struct BuffMessage {
    std::uint32_t robot_id = 0;
    BuffRecord record;
};

struct SentryCtrlResult {
    std::uint32_t command_id = 0;
    std::uint32_t result_code = 0;
};

struct PathPlan {
    std::uint32_t intention = 0;
    std::uint32_t sender_id = 0;
    std::vector<std::pair<double, double>> points;  // 米
};

using MapClick = MapMark;

// CustomByteBlock 里解出来的一包雷达情报。*_age_ms 是终端收到时这段已经多旧(雷达填的毫秒
// 加车填的转发延迟),空 = 雷达还没收到过这段,这段的内容不能用。body 的时刻和新鲜度由 Store 填。
struct RadarIntel {
    std::optional<std::uint32_t> ammo_age_ms, field_age_ms, buffs_age_ms;
    ::rm_terminal::RadarIntel body;
};

}
