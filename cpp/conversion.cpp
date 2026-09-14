#include "conversion.h"

#include <cmath>

namespace rm_terminal {
namespace {
template<class T, class Predicate>
bool merge(Field<T>& field, const std::optional<T>& value, MonotonicMs now, Predicate valid) {
    if (!value) return true;
    if (!valid(*value)) {
        field.quality = Quality::Invalid;
        return false;
    }
    field.value = value;
    field.last_valid = now;
    field.quality = Quality::Valid;
    return true;
}
const auto any_value = [](const auto&) { return true; };
const auto finite = [](double value) { return std::isfinite(value); };
const auto nonnegative = [](double value) { return std::isfinite(value) && value >= 0; };
auto between(double low, double high) {
    return [=](double value) { return std::isfinite(value) && value >= low && value <= high; };
}
}

// Evaluate every present field even when another field is invalid.
#define MERGE(name, predicate) ok = merge(state.name, patch.name, now, predicate) && ok

bool convert(GameFields<Field>& state, const inbound::GameStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(current_round, [](auto v) { return v > 0; });
    MERGE(total_rounds, [](auto v) { return v > 0; });
    MERGE(red_score, any_value);
    MERGE(blue_score, any_value);
    MERGE(current_stage, between(0, 5));
    MERGE(stage_countdown_sec, nonnegative);
    MERGE(stage_elapsed_sec, nonnegative);
    MERGE(is_paused, any_value);
    MERGE(winner, [](auto v) { return v <= 2 || v == 255; });
    MERGE(end_reason, between(0, 255));
    return ok;
}
bool convert(DynamicFields<Field>& state, const inbound::RobotDynamicStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(current_hp, any_value);
    MERGE(max_hp, any_value);
    MERGE(shooter_heat_17mm, any_value);
    MERGE(shooter_heat_limit, any_value);
    MERGE(bullet_speed, nonnegative);
    MERGE(remaining_ammo, any_value);
    MERGE(coin, any_value);
    MERGE(chassis_power, nonnegative);
    MERGE(buffer_energy, nonnegative);
    return ok;
}
bool convert(ModuleFields<Field>& state, const inbound::RobotModuleStatus& patch, MonotonicMs now) {
    bool ok = true;
    const auto module = between(0, 2);
    MERGE(power_manager, module);
    MERGE(rfid, module);
    MERGE(light_strip, module);
    MERGE(small_shooter, module);
    MERGE(big_shooter, module);
    MERGE(uwb, module);
    MERGE(armor, module);
    MERGE(video_transmission, module);
    MERGE(capacitor, module);
    MERGE(main_controller, module);
    MERGE(laser_detection_module, module);
    return ok;
}
bool convert(PositionFields<Field>& state, const inbound::RobotPosition& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(x, finite);
    MERGE(y, finite);
    MERGE(yaw, finite);
    return ok;
}
bool convert(EventFields<Field>& state, const inbound::Event& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(timestamp_ms, any_value);
    MERGE(level, between(0, 3));
    MERGE(text, [](const auto& v) { return v.size() <= 4096; });
    return ok;
}
bool convert(TelemetryFields<Field>& state, const inbound::RobotTelemetry& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(sequence, any_value);
    MERGE(timestamp_us, any_value);
    MERGE(gimbal_yaw, finite);
    MERGE(gimbal_pitch, finite);
    MERGE(chassis_mode, any_value);
    MERGE(vision_online, any_value);
    MERGE(autoaim_enabled, any_value);
    MERGE(target_locked, any_value);
    MERGE(target_id, any_value);
    MERGE(target_distance, nonnegative);
    MERGE(target_yaw_err, finite);
    MERGE(target_pitch_err, finite);
    MERGE(confidence, between(0, 1));
    MERGE(friction_rpm, nonnegative);
    MERGE(fire_permit, any_value);
    MERGE(bbox_cx, between(0, 1920));
    MERGE(bbox_cy, between(0, 1080));
    MERGE(bbox_w, between(0, 1920));
    MERGE(bbox_h, between(0, 1080));
    return ok;
}
#undef MERGE
}
