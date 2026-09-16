#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace rm_terminal {

using MonotonicMs = std::int64_t;
struct RobotId {
    std::uint32_t value;
    bool operator<(RobotId other) const { return value < other.value; }
};
enum class Quality { Missing, Valid, Invalid };
enum class Freshness { NeverReceived, Fresh, Stale };

template<class T> struct Field {
    std::optional<T> value;
    std::optional<MonotonicMs> last_valid;
    Quality quality = Quality::Missing;
    Freshness freshness = Freshness::NeverReceived;
};

// The same field vocabulary supports optional patches and timestamped domain values.
template<template<class> class F> struct GameFields {
    F<std::uint32_t> current_round, total_rounds, red_score, blue_score, current_stage;
    F<std::int32_t> stage_countdown_sec, stage_elapsed_sec;
    F<bool> is_paused;
    F<std::uint32_t> winner, end_reason;
    F<std::uint32_t> red_base_hp, red_base_max_hp, blue_base_hp, blue_base_max_hp;
    F<std::uint32_t> red_outpost_hp, red_outpost_max_hp, blue_outpost_hp, blue_outpost_max_hp;
    F<std::string> red_team_name, blue_team_name;
    F<std::uint32_t> red_economy, blue_economy, red_total_damage, blue_total_damage;
    F<std::uint32_t> red_fortress_sec, blue_fortress_sec, fortress_holder;
    auto fields() { return std::tie(current_round, total_rounds, red_score, blue_score,
        current_stage, stage_countdown_sec, stage_elapsed_sec, is_paused, winner, end_reason,
        red_base_hp, red_base_max_hp, blue_base_hp, blue_base_max_hp,
        red_outpost_hp, red_outpost_max_hp, blue_outpost_hp, blue_outpost_max_hp,
        red_team_name, blue_team_name,
        red_economy, blue_economy, red_total_damage, blue_total_damage,
        red_fortress_sec, blue_fortress_sec, fortress_holder); }
};
template<template<class> class F> struct DynamicFields {
    F<std::uint32_t> current_hp, max_hp, shooter_heat_17mm, shooter_heat_limit;
    F<double> bullet_speed;
    F<std::uint32_t> remaining_ammo, coin;
    F<double> chassis_power, buffer_energy;
    auto fields() { return std::tie(current_hp, max_hp, shooter_heat_17mm, shooter_heat_limit,
        bullet_speed, remaining_ammo, coin, chassis_power, buffer_energy); }
};
template<template<class> class F> struct ModuleFields {
    F<std::uint32_t> power_manager, rfid, light_strip, small_shooter, big_shooter,
        uwb, armor, video_transmission, capacitor, main_controller, laser_detection_module;
    auto fields() { return std::tie(power_manager, rfid, light_strip, small_shooter,
        big_shooter, uwb, armor, video_transmission, capacitor, main_controller, laser_detection_module); }
};
template<template<class> class F> struct PositionFields {
    F<double> x, y, yaw;
    auto fields() { return std::tie(x, y, yaw); }
};
template<template<class> class F> struct BlindFields {
    F<bool> self_base_blinded;
    F<std::uint64_t> blind_started_ms;
    F<std::int32_t> blind_remaining_ms;
    F<std::uint32_t> cause;
    auto fields() { return std::tie(self_base_blinded, blind_started_ms, blind_remaining_ms, cause); }
};
template<template<class> class F> struct EventFields {
    F<std::uint64_t> timestamp_ms;
    F<std::uint32_t> level;
    F<std::string> text;
    auto fields() { return std::tie(timestamp_ms, level, text); }
};
template<template<class> class F> struct TelemetryFields {
    F<std::uint32_t> sequence;
    F<std::uint64_t> timestamp_us;
    F<double> gimbal_yaw, gimbal_pitch;
    F<std::uint32_t> chassis_mode;
    F<bool> vision_online, autoaim_enabled, target_locked;
    F<std::uint32_t> target_id;
    F<double> target_distance, target_yaw_err, target_pitch_err, confidence, friction_rpm;
    F<bool> fire_permit;
    F<double> bbox_cx, bbox_cy, bbox_w, bbox_h;
    auto fields() { return std::tie(sequence, timestamp_us, gimbal_yaw, gimbal_pitch,
        chassis_mode, vision_online, autoaim_enabled, target_locked, target_id,
        target_distance, target_yaw_err, target_pitch_err, confidence, friction_rpm,
        fire_permit, bbox_cx, bbox_cy, bbox_w, bbox_h); }
};

struct EventRecord {
    std::uint64_t timestamp_ms;
    std::uint32_t level;
    std::string text;
    MonotonicMs received_at;
};

// Ring buffer, bounded so a match-long run cannot grow without limit. Overwrites
// the oldest record when full and counts the loss in droppedCount(), so the UI can
// disclose truncation instead of implying the history is complete.
class EventHistory {
public:
    explicit EventHistory(std::size_t capacity = 50);

    void push(EventRecord record);
    // Newest first. Pinned by test, because a reversed order silently puts the
    // oldest alert at the top of an alert panel.
    std::vector<EventRecord> recent(std::size_t max) const;

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return buffer_.size(); }
    std::size_t droppedCount() const { return dropped_; }

private:
    std::vector<EventRecord> buffer_;
    std::size_t next_ = 0;
    std::size_t size_ = 0;
    std::size_t dropped_ = 0;
};

struct RobotState {
    DynamicFields<Field> dynamic;
    ModuleFields<Field> modules;
    PositionFields<Field> position;
    TelemetryFields<Field> telemetry;
};

// One robot as the tactical map needs it. `position` keeps the full Field
// vocabulary so a missing coordinate stays distinguishable from a real origin,
// which is what stops an unlocated robot from being drawn at the field corner.
struct MapRobot {
    RobotId id;
    std::uint32_t faction = 0;
    bool is_self = false;
    PositionFields<Field> position;
};

// One robot's health as the top bar needs it. Health keeps the full Field
// vocabulary so a missing HP stays distinguishable from a real 0, which is what
// stops a robot with no data from being drawn as dead.
struct RobotHealth {
    RobotId id;
    std::uint32_t faction = 0;
    Field<std::uint32_t> current_hp;
    Field<std::uint32_t> max_hp;
};

struct Snapshot {
    GameFields<Field> game;
    // Retained alongside `events`: the existing single-event panel, convert()
    // overload, and domain tests all depend on this field.
    EventFields<Field> event;
    EventHistory events;
    BlindFields<Field> blind;
    // Sorted by ascending id and replaced wholesale on every position set, so
    // the drawing order is stable across frames and the list stays bounded.
    std::vector<MapRobot> map_robots;
    std::size_t map_invalid_entries = 0;
    // Sorted by ascending id and replaced wholesale on every health set, same
    // contract as map_robots.
    std::vector<RobotHealth> robot_health;
    std::map<RobotId, RobotState> robots;
};

}
