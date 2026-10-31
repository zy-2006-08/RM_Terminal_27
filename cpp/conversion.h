#pragma once

#include "inbound.h"

namespace rm_terminal {

// 把补丁里出现的字段逐个校验后并入状态。不合法的字段标成 Invalid、保留上一次的合法值;
// 返回值为假表示至少有一个字段被拒。
bool convert(GameFields<Field>& state, const inbound::GameStatus& patch, MonotonicMs now);
bool convert(DynamicFields<Field>& state, const inbound::RobotDynamicStatus& patch, MonotonicMs now);
bool convert(StaticFields<Field>& state, const inbound::RobotStaticStatus& patch, MonotonicMs now);
bool convert(ModuleFields<Field>& state, const inbound::RobotModuleStatus& patch, MonotonicMs now);
bool convert(PositionFields<Field>& state, const inbound::RobotPosition& patch, MonotonicMs now);
bool convert(RespawnFields<Field>& state, const inbound::RobotRespawnStatus& patch, MonotonicMs now);
bool convert(TechCoreFields<Field>& state, const inbound::TechCoreStatus& patch, MonotonicMs now);
bool convert(DartFields<Field>& state, const inbound::DartStatus& patch, MonotonicMs now);
bool convert(SentryFields<Field>& state, const inbound::SentryStatus& patch, MonotonicMs now);

}
