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

}
