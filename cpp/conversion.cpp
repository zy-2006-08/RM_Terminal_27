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
const auto positive = [](auto value) { return value > 0; };
auto between(double low, double high) {
    return [=](double value) { return std::isfinite(value) && value >= low && value <= high; };
}
}

// Evaluate every present field even when another field is invalid.
#define MERGE(name, predicate) ok = merge(state.name, patch.name, now, predicate) && ok

bool convert(GameFields<Field>& state, const inbound::GameStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(current_round, positive);
    MERGE(total_rounds, positive);
    MERGE(red_score, any_value);
    MERGE(blue_score, any_value);
    MERGE(current_stage, between(0, 5));
    MERGE(stage_countdown_sec, nonnegative);
    MERGE(stage_elapsed_sec, nonnegative);
    MERGE(is_paused, any_value);
    MERGE(winner, [](auto v) { return v <= 2 || v == 255; });
    MERGE(end_reason, between(0, 255));
    // 漏掉一个 MERGE 不报错:字段停在 NeverReceived,界面谎报「无数据」。max 为 0
    // 会让血条除零,故按无效拒绝。
    MERGE(red_base_hp, any_value);
    MERGE(red_base_max_hp, positive);
    MERGE(blue_base_hp, any_value);
    MERGE(blue_base_max_hp, positive);
    MERGE(red_base_status, between(0, 2));
    MERGE(blue_base_status, between(0, 2));
    MERGE(red_outpost_hp, any_value);
    MERGE(red_outpost_max_hp, positive);
    MERGE(blue_outpost_hp, any_value);
    MERGE(blue_outpost_max_hp, positive);
    // 阵地状态只有 0..4 五个合法值,未知编码一律拒绝:画成「未控」会谎报战场。
    const auto zone = between(0, 4);
    MERGE(red_outpost_zone, zone);
    MERGE(blue_outpost_zone, zone);
    MERGE(red_base_zone, zone);
    MERGE(blue_base_zone, zone);
    MERGE(red_total_damage, any_value);
    MERGE(blue_total_damage, any_value);
    MERGE(red_control_points, any_value);
    MERGE(blue_control_points, any_value);
    MERGE(red_fortress_activation_sec, nonnegative);
    MERGE(blue_fortress_activation_sec, nonnegative);
    MERGE(red_outpost_zone_progress, any_value);
    MERGE(blue_outpost_zone_progress, any_value);
    MERGE(red_base_zone_progress, any_value);
    MERGE(blue_base_zone_progress, any_value);
    MERGE(economy, any_value);
    MERGE(total_economy, any_value);
    MERGE(tech_level, any_value);
    MERGE(encryption_level, any_value);
    MERGE(rune_status, between(0, 3));
    MERGE(rune_activated_arms, any_value);
    MERGE(rune_average_rings, nonnegative);
    MERGE(selected_shooter, between(1, 2));
    MERGE(selected_chassis, between(1, 2));
    return ok;
}
bool convert(DynamicFields<Field>& state, const inbound::RobotDynamicStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(current_hp, any_value);
    MERGE(heat, nonnegative);
    MERGE(bullet_speed, nonnegative);
    MERGE(buffer_energy, any_value);
    MERGE(experience, any_value);
    MERGE(experience_for_upgrade, any_value);
    MERGE(total_fired, any_value);
    MERGE(remaining_ammo, any_value);
    MERGE(out_of_combat, any_value);
    MERGE(out_of_combat_countdown, any_value);
    MERGE(can_remote_heal, any_value);
    MERGE(can_remote_ammo, any_value);
    return ok;
}
bool convert(StaticFields<Field>& state, const inbound::RobotStaticStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(connection_state, between(0, 1));
    MERGE(field_state, between(0, 1));
    MERGE(alive_state, between(0, 2));
    MERGE(robot_id, positive);
    MERGE(robot_type, any_value);
    MERGE(shooter_system, any_value);
    MERGE(chassis_system, any_value);
    MERGE(level, any_value);
    MERGE(max_hp, positive);
    MERGE(max_heat, any_value);
    MERGE(heat_cooldown_rate, nonnegative);
    MERGE(max_power, any_value);
    MERGE(max_buffer_energy, any_value);
    MERGE(max_chassis_energy, any_value);
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
    MERGE(modular_42mm, between(0, 1));
    return ok;
}
bool convert(PositionFields<Field>& state, const inbound::RobotPosition& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(x, finite);
    MERGE(y, finite);
    MERGE(yaw, finite);
    return ok;
}
bool convert(RespawnFields<Field>& state, const inbound::RobotRespawnStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(pending, any_value);
    MERGE(total_progress, any_value);
    MERGE(current_progress, any_value);
    MERGE(can_free, any_value);
    MERGE(gold_cost, any_value);
    MERGE(can_pay, any_value);
    return ok;
}
bool convert(TechCoreFields<Field>& state, const inbound::TechCoreStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(maximum_difficulty, any_value);
    MERGE(basic_state, between(0, 3));
    MERGE(putin_state, between(0, 1));
    MERGE(move_state, between(0, 1));
    MERGE(rotate_state, between(0, 1));
    MERGE(enemy_core_status, between(0, 2));
    MERGE(remain_time_all, any_value);
    MERGE(remain_time_step, any_value);
    return ok;
}
bool convert(DartFields<Field>& state, const inbound::DartStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(target_id, between(0, 5));
    MERGE(gate, between(0, 2));
    return ok;
}
bool convert(SentryFields<Field>& state, const inbound::SentryStatus& patch, MonotonicMs now) {
    bool ok = true;
    MERGE(posture, between(0, 3));
    MERGE(weakened, any_value);
    MERGE(powered, any_value);
    return ok;
}
#undef MERGE
}
