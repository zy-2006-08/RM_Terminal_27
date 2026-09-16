#pragma once

#include "domain.h"

namespace rm_terminal::inbound {

// Local simulation vocabulary only; not a protobuf decoder or official 2027 contract.
using GameStatus = GameFields<std::optional>;
struct RobotDynamicStatus : DynamicFields<std::optional> {
    std::optional<std::uint32_t> robot_id;
};
using RobotModuleStatus = ModuleFields<std::optional>;
using RobotPosition = PositionFields<std::optional>;
using Event = EventFields<std::optional>;
using RobotTelemetry = TelemetryFields<std::optional>;
using BlindStatus = BlindFields<std::optional>;
struct RobotPositionEntry : PositionFields<std::optional> {
    std::optional<std::uint32_t> robot_id, faction;
    std::optional<bool> is_self;
};
struct RobotPositionSet {
    std::vector<RobotPositionEntry> entries;
};
struct RobotHealthEntry {
    std::optional<std::uint32_t> robot_id, faction, current_hp, max_hp;
};
struct RobotHealthSet {
    std::vector<RobotHealthEntry> entries;
};

}
