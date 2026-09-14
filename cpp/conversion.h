#pragma once

#include "inbound.h"

namespace rm_terminal {

bool convert(GameFields<Field>& state, const inbound::GameStatus& patch, MonotonicMs now);
bool convert(DynamicFields<Field>& state, const inbound::RobotDynamicStatus& patch, MonotonicMs now);
bool convert(ModuleFields<Field>& state, const inbound::RobotModuleStatus& patch, MonotonicMs now);
bool convert(PositionFields<Field>& state, const inbound::RobotPosition& patch, MonotonicMs now);
bool convert(EventFields<Field>& state, const inbound::Event& patch, MonotonicMs now);
bool convert(TelemetryFields<Field>& state, const inbound::RobotTelemetry& patch, MonotonicMs now);
bool convert(BlindFields<Field>& state, const inbound::BlindStatus& patch, MonotonicMs now);

}
