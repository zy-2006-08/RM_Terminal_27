#include "store.h"

#include <stdexcept>

namespace rm_terminal {
namespace {
template<class Group>
void age(Group& group, MonotonicMs now, MonotonicMs threshold) {
    std::apply([=](auto&... field) {
        ((field.freshness = !field.last_valid ? Freshness::NeverReceived :
            now - *field.last_valid >= threshold ? Freshness::Stale : Freshness::Fresh), ...);
    }, group.fields());
}
bool valid_source(std::optional<RobotId> source) { return source && source->value > 0; }
}

Store::Store(MonotonicMs stale_after_ms) : threshold_(stale_after_ms) {
    if (threshold_ <= 0) throw std::invalid_argument("freshness threshold must be positive");
}
bool Store::advance(MonotonicMs now) {
    if (now < latest_) return false;
    latest_ = now;
    return true;
}
bool Store::apply(const inbound::GameStatus& patch, MonotonicMs now) {
    return advance(now) && convert(state_.game, patch, now);
}
bool Store::apply(const inbound::Event& patch, MonotonicMs now) {
    return advance(now) && convert(state_.event, patch, now);
}
bool Store::apply(const inbound::RobotDynamicStatus& patch, std::optional<RobotId> source, MonotonicMs now) {
    if (patch.robot_id) {
        if (source && source->value != *patch.robot_id) return false;
        source = RobotId{*patch.robot_id};
    }
    return valid_source(source) && advance(now) && convert(state_.robots[*source].dynamic, patch, now);
}
bool Store::apply(const inbound::RobotModuleStatus& patch, std::optional<RobotId> source, MonotonicMs now) {
    return valid_source(source) && advance(now) && convert(state_.robots[*source].modules, patch, now);
}
bool Store::apply(const inbound::RobotPosition& patch, std::optional<RobotId> source, MonotonicMs now) {
    return valid_source(source) && advance(now) && convert(state_.robots[*source].position, patch, now);
}
bool Store::apply(const inbound::RobotTelemetry& patch, std::optional<RobotId> source, MonotonicMs now) {
    return valid_source(source) && advance(now) && convert(state_.robots[*source].telemetry, patch, now);
}
Snapshot Store::snapshot(MonotonicMs now) const {
    if (now < latest_) throw std::invalid_argument("snapshot precedes latest update");
    auto copy = state_;
    age(copy.game, now, threshold_);
    age(copy.event, now, threshold_);
    for (auto& entry : copy.robots) {
        auto& robot = entry.second;
        age(robot.dynamic, now, threshold_);
        age(robot.modules, now, threshold_);
        age(robot.position, now, threshold_);
        age(robot.telemetry, now, threshold_);
    }
    return copy;
}
}
