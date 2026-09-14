#include "store.h"

#include <stdexcept>

namespace rm_terminal {

EventHistory::EventHistory(std::size_t capacity) : buffer_(capacity == 0 ? 1 : capacity) {}

void EventHistory::push(EventRecord record) {
    if (size_ == buffer_.size()) ++dropped_;
    buffer_[next_] = std::move(record);
    next_ = (next_ + 1) % buffer_.size();
    if (size_ < buffer_.size()) ++size_;
}

std::vector<EventRecord> EventHistory::recent(std::size_t max) const {
    const std::size_t count = max < size_ ? max : size_;
    std::vector<EventRecord> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Walk backwards from the most recently written slot so the newest record
        // comes out first. `+ buffer_.size()` keeps the modulo operand positive.
        const std::size_t index = (next_ + buffer_.size() - 1 - i) % buffer_.size();
        result.push_back(buffer_[index]);
    }
    return result;
}

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

Store::Store(MonotonicMs stale_after_ms, std::size_t event_capacity) : threshold_(stale_after_ms) {
    if (threshold_ <= 0) throw std::invalid_argument("freshness threshold must be positive");
    state_.events = EventHistory(event_capacity);
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
    if (!(advance(now) && convert(state_.event, patch, now))) return false;

    // Republished at 5Hz, so dedup is required or one alert fills the history.
    // The key is the full triple, not just timestamp: two distinct events can share
    // a millisecond at the start of blinding, and the discarded one could be the
    // severe alert.
    if (!patch.text) return true;
    const std::uint64_t timestamp = patch.timestamp_ms.value_or(0);
    const std::uint32_t level = patch.level.value_or(0);
    if (last_event_ && last_event_->timestamp_ms == timestamp &&
        last_event_->level == level && last_event_->text == *patch.text) {
        return true;
    }
    last_event_ = EventKey{timestamp, level, *patch.text};
    state_.events.push(EventRecord{timestamp, level, *patch.text, now});
    return true;
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
