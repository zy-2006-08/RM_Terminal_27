#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>

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
    auto fields() { return std::tie(current_round, total_rounds, red_score, blue_score,
        current_stage, stage_countdown_sec, stage_elapsed_sec, is_paused, winner, end_reason); }
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

struct RobotState {
    DynamicFields<Field> dynamic;
    ModuleFields<Field> modules;
    PositionFields<Field> position;
    TelemetryFields<Field> telemetry;
};
struct Snapshot {
    GameFields<Field> game;
    EventFields<Field> event;
    std::map<RobotId, RobotState> robots;
};

}
