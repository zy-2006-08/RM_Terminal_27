#include "store.h"

#include <algorithm>
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
    state_.events.push(
        EventRecord{timestamp, level, *patch.text, now, patch.faction.value_or(0)});
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
bool Store::apply(const inbound::BlindStatus& patch, MonotonicMs now) {
    return advance(now) && convert(state_.blind, patch, now);
}
bool Store::apply(const inbound::RobotPositionSet& patch, MonotonicMs now) {
    if (!advance(now)) return false;
    bool ok = true;

    // Only one robot can be us. Extra claims are demoted rather than trusted,
    // because picking arbitrarily would attach our own authoritative coordinates
    // to somebody else's marker.
    std::size_t self_claims = 0;
    std::optional<std::uint32_t> self_id;
    for (const auto& entry : patch.entries) {
        if (!entry.robot_id || !entry.is_self.value_or(false)) continue;
        ++self_claims;
        if (!self_id || *entry.robot_id < *self_id) self_id = entry.robot_id;
    }
    if (self_claims > 1) {
        state_.map_invalid_entries += self_claims - 1;
        ok = false;
    }

    std::vector<MapRobot> robots;
    robots.reserve(patch.entries.size());
    for (const auto& entry : patch.entries) {
        if (!entry.robot_id) {
            ++state_.map_invalid_entries;
            ok = false;
            continue;
        }
        MapRobot robot;
        robot.id = RobotId{*entry.robot_id};
        // Never inferred from robot_id: the 2027 id encoding is unknown, so
        // guessing a side would report an alliance with false confidence.
        if (entry.faction) {
            if (*entry.faction <= 2) {
                robot.faction = *entry.faction;
            } else {
                ++state_.map_invalid_entries;
                ok = false;
            }
        }
        robot.is_self = self_id && *self_id == *entry.robot_id;
        if (robot.is_self) {
            // The single-robot path is the sole authority for our own coordinates.
            // Coordinates carried in the set are dropped, so a simulator (or a
            // spoofed publisher) cannot move our own marker. Absent authoritative
            // data leaves the position Missing rather than falling back.
            const auto existing = state_.robots.find(robot.id);
            if (existing != state_.robots.end()) robot.position = existing->second.position;
        } else if (!convert(robot.position, entry, now)) {
            ++state_.map_invalid_entries;
            ok = false;
        }
        robots.push_back(std::move(robot));
    }
    std::stable_sort(robots.begin(), robots.end(),
                     [](const MapRobot& left, const MapRobot& right) { return left.id < right.id; });
    state_.map_robots = std::move(robots);
    return ok;
}
bool Store::apply(const inbound::RobotHealthSet& patch, MonotonicMs now) {
    if (!advance(now)) return false;
    bool ok = true;

    std::vector<RobotHealth> health;
    health.reserve(patch.entries.size());
    for (const auto& entry : patch.entries) {
        if (!entry.robot_id) {
            ok = false;
            continue;
        }
        RobotHealth robot;
        robot.id = RobotId{*entry.robot_id};
        // Same rule as the position set: a side is never inferred from robot_id,
        // because the 2027 id encoding is unknown.
        if (entry.faction) {
            if (*entry.faction <= 2) {
                robot.faction = *entry.faction;
            } else {
                ok = false;
            }
        }
        // max_hp == 0 would make any bar arithmetic divide by zero downstream, so
        // it is rejected as invalid rather than stored.
        const bool max_ok = !entry.max_hp || *entry.max_hp > 0;
        if (!max_ok) ok = false;
        if (entry.current_hp) {
            robot.current_hp.value = *entry.current_hp;
            robot.current_hp.last_valid = now;
            robot.current_hp.quality = Quality::Valid;
            robot.current_hp.freshness = Freshness::Fresh;
        }
        if (entry.max_hp && max_ok) {
            robot.max_hp.value = *entry.max_hp;
            robot.max_hp.last_valid = now;
            robot.max_hp.quality = Quality::Valid;
            robot.max_hp.freshness = Freshness::Fresh;
        }
        health.push_back(std::move(robot));
    }
    std::stable_sort(health.begin(), health.end(),
                     [](const RobotHealth& left, const RobotHealth& right) { return left.id < right.id; });
    state_.robot_health = std::move(health);
    return ok;
}
Snapshot Store::snapshot(MonotonicMs now) const {
    if (now < latest_) throw std::invalid_argument("snapshot precedes latest update");
    auto copy = state_;
    age(copy.game, now, threshold_);
    age(copy.event, now, threshold_);
    age(copy.blind, now, threshold_);
    for (auto& robot : copy.map_robots) age(robot.position, now, threshold_);
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
