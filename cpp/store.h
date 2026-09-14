#pragma once

#include "conversion.h"

namespace rm_terminal {

class Store {
public:
    explicit Store(MonotonicMs stale_after_ms);
    bool apply(const inbound::GameStatus& patch, MonotonicMs now);
    bool apply(const inbound::Event& patch, MonotonicMs now);
    bool apply(const inbound::RobotDynamicStatus& patch, std::optional<RobotId> source, MonotonicMs now);
    bool apply(const inbound::RobotModuleStatus& patch, std::optional<RobotId> source, MonotonicMs now);
    bool apply(const inbound::RobotPosition& patch, std::optional<RobotId> source, MonotonicMs now);
    bool apply(const inbound::RobotTelemetry& patch, std::optional<RobotId> source, MonotonicMs now);
    Snapshot snapshot(MonotonicMs now) const;

private:
    bool advance(MonotonicMs now);
    Snapshot state_;
    MonotonicMs threshold_;
    MonotonicMs latest_ = 0;
};

}
