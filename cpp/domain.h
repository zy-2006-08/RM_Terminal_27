#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
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

// 阵营。2027 协议附录二:红方 1–11,蓝方 101–111(蓝 = 红 + 100)。
constexpr std::uint32_t kRed = 1;
constexpr std::uint32_t kBlue = 2;
inline std::uint32_t faction_of(std::uint32_t robot_id) { return robot_id > 100 ? kBlue : kRed; }
inline std::uint32_t opponent_of(std::uint32_t faction) { return faction == kRed ? kBlue : kRed; }
// 有血量的场上机器人,也是 GlobalUnitStatus.robot_health 和雷达消息的排列顺序:
// 1 重装、3/4 步兵、7 哨兵。6 空中不在其中。
constexpr std::array<std::uint32_t, 4> kFieldRobots{1, 3, 4, 7};

// The same field vocabulary supports optional patches and timestamped domain values.
// 一律按红蓝存:协议里「己方/对方」的字段在解码时按席位换算好,界面不必再知道自己是哪方。
template<template<class> class F> struct GameFields {
    // GameStatus,5Hz
    F<std::uint32_t> current_round, total_rounds, red_score, blue_score, current_stage;
    F<std::int32_t> stage_countdown_sec, stage_elapsed_sec;
    F<bool> is_paused;
    F<std::uint32_t> winner, end_reason;
    // GlobalUnitStatus,1Hz。上限血量协议不发,解码时按规则手册定值填(基地 10000、前哨站 1000)。
    F<std::uint32_t> red_base_hp, red_base_max_hp, blue_base_hp, blue_base_max_hp;
    // 0 有 100% 防御增益,1 无增益且护甲未展开,2 无增益且护甲展开
    F<std::uint32_t> red_base_status, blue_base_status;
    F<std::uint32_t> red_outpost_hp, red_outpost_max_hp, blue_outpost_hp, blue_outpost_max_hp;
    // 阵地控制,换成和 Event 5–8 相同的绝对编码:0 未控 1 红方临时 2 红方完全 3 蓝方临时 4 蓝方完全
    F<std::uint32_t> red_outpost_zone, blue_outpost_zone, red_base_zone, blue_base_zone;
    F<std::uint32_t> red_total_damage, blue_total_damage;
    F<std::uint32_t> red_control_points, blue_control_points;
    // GlobalSpecialMechanism,1Hz。消息只列正在生效的机制,没列出的按 0 记。
    // 堡垒:这一方的堡垒正被对方激活了几秒(满 10 秒这一方的基地护甲展开)。
    F<std::int32_t> red_fortress_activation_sec, blue_fortress_activation_sec;
    // 阵地激活计时:>0 红方在推进,<0 蓝方在推进,0 没有变化。
    F<std::int32_t> red_outpost_zone_progress, blue_outpost_zone_progress,
        red_base_zone_progress, blue_base_zone_progress;
    // GlobalLogisticsStatus,1Hz,只有己方
    F<std::uint32_t> economy;
    F<std::uint64_t> total_economy;
    F<std::uint32_t> tech_level, encryption_level;
    // RuneStatusSync,1Hz,己方:1 未激活 2 正在激活 3 已激活
    F<std::uint32_t> rune_status, rune_activated_arms;
    F<double> rune_average_rings;
    // RobotPerformanceSelectionSync,1Hz,自机:发射 1 冷却优先 2 爆发优先;底盘 1 血量优先 2 功率优先
    F<std::uint32_t> selected_shooter, selected_chassis;
    auto fields() { return std::tie(current_round, total_rounds, red_score, blue_score,
        current_stage, stage_countdown_sec, stage_elapsed_sec, is_paused, winner, end_reason,
        red_base_hp, red_base_max_hp, blue_base_hp, blue_base_max_hp,
        red_base_status, blue_base_status,
        red_outpost_hp, red_outpost_max_hp, blue_outpost_hp, blue_outpost_max_hp,
        red_outpost_zone, blue_outpost_zone, red_base_zone, blue_base_zone,
        red_total_damage, blue_total_damage, red_control_points, blue_control_points,
        red_fortress_activation_sec, blue_fortress_activation_sec,
        red_outpost_zone_progress, blue_outpost_zone_progress,
        red_base_zone_progress, blue_base_zone_progress,
        economy, total_economy, tech_level, encryption_level,
        rune_status, rune_activated_arms, rune_average_rings,
        selected_shooter, selected_chassis); }
};
// RobotDynamicStatus,10Hz,只有自机。
template<template<class> class F> struct DynamicFields {
    F<std::uint32_t> current_hp;
    F<double> heat, bullet_speed;
    F<std::uint32_t> buffer_energy, experience, experience_for_upgrade, total_fired, remaining_ammo;
    F<bool> out_of_combat;
    F<std::uint32_t> out_of_combat_countdown;
    F<bool> can_remote_heal, can_remote_ammo;
    auto fields() { return std::tie(current_hp, heat, bullet_speed, buffer_energy, experience,
        experience_for_upgrade, total_fired, remaining_ammo, out_of_combat,
        out_of_combat_countdown, can_remote_heal, can_remote_ammo); }
};
// RobotStaticStatus,1Hz,只有自机。robot_id 用来核对开机选的席位。
template<template<class> class F> struct StaticFields {
    F<std::uint32_t> connection_state, field_state, alive_state, robot_id, robot_type,
        shooter_system, chassis_system, level, max_hp, max_heat;
    F<double> heat_cooldown_rate;
    F<std::uint32_t> max_power, max_buffer_energy, max_chassis_energy;
    auto fields() { return std::tie(connection_state, field_state, alive_state, robot_id,
        robot_type, shooter_system, chassis_system, level, max_hp, max_heat, heat_cooldown_rate,
        max_power, max_buffer_energy, max_chassis_energy); }
};
template<template<class> class F> struct ModuleFields {
    F<std::uint32_t> power_manager, rfid, light_strip, small_shooter, big_shooter,
        uwb, armor, video_transmission, capacitor, main_controller;
    // 仅重装:有没有模块化 42mm 发射机构的安装权限(0/1),不是在线状态,不计入模块在线数。
    F<std::uint32_t> modular_42mm;
    auto fields() { return std::tie(power_manager, rfid, light_strip, small_shooter,
        big_shooter, uwb, armor, video_transmission, capacitor, main_controller, modular_42mm); }
};
template<template<class> class F> struct PositionFields {
    F<double> x, y, yaw;
    auto fields() { return std::tie(x, y, yaw); }
};
// RobotRespawnStatus,1Hz,只有自机。
template<template<class> class F> struct RespawnFields {
    F<bool> pending;
    F<std::uint32_t> total_progress, current_progress;
    F<bool> can_free;
    F<std::uint32_t> gold_cost;
    F<bool> can_pay;
    auto fields() { return std::tie(pending, total_progress, current_progress, can_free,
        gold_cost, can_pay); }
};
// 我方操作界面被飞镖遮挡的状态。2027 协议没有这条消息:由 Store 按 Event 9(飞镖命中)
// 和规则手册 5.7.3 的时长在本机推算,每次取快照时现算,见 store.cpp。
template<template<class> class F> struct BlindFields {
    F<bool> self_base_blinded;
    F<std::int32_t> blind_remaining_ms;
    F<std::uint32_t> target;  // 最近一次让我方被遮挡的飞镖目标,1–5
    auto fields() { return std::tie(self_base_blinded, blind_remaining_ms, target); }
};
// TechCoreMotionStateSync,1Hz,重装装配。
template<template<class> class F> struct TechCoreFields {
    F<std::uint32_t> maximum_difficulty, basic_state, putin_state, move_state, rotate_state,
        enemy_core_status, remain_time_all, remain_time_step;
    auto fields() { return std::tie(maximum_difficulty, basic_state, putin_state, move_state,
        rotate_state, enemy_core_status, remain_time_all, remain_time_step); }
};
// DartSelectTargetStatusSync,1Hz,指挥手。gate:0 关闭 1 开启中 2 已开启。
template<template<class> class F> struct DartFields {
    F<std::uint32_t> target_id, gate;
    auto fields() { return std::tie(target_id, gate); }
};
// SentryStatusSync,1Hz,指挥手。posture:1 进攻 2 防御 3 移动。
template<template<class> class F> struct SentryFields {
    F<std::uint32_t> posture;
    F<bool> weakened, powered;
    auto fields() { return std::tie(posture, weakened, powered); }
};

struct EventRecord {
    std::int32_t event_id = 0;
    std::uint32_t level = 0;  // 0 信息 1 提示 2 警告 3 严重
    std::string text;
    MonotonicMs received_at = 0;
    std::uint32_t faction = 0;  // 横幅配色:0 中立 1 红 2 蓝
    // 收到时的比赛倒计时。协议不带时间戳;记比赛时间而不是墙上时间,录像回放时才显示得对。
    std::optional<std::int32_t> countdown_sec;
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
    StaticFields<Field> statics;
    ModuleFields<Field> modules;
    PositionFields<Field> position;
    RespawnFields<Field> respawn;
};

// One robot as the tactical map needs it. `position` keeps the full Field
// vocabulary so a missing coordinate stays distinguishable from a real origin,
// which is what stops an unlocated robot from being drawn at the field corner.
struct MapRobot {
    RobotId id;
    std::uint32_t faction = 0;
    bool is_self = false;
    // 坐标来自雷达(RadarInfoToClient,别的车只有这一个来源),否则来自服务器的
    // RobotPosition(自机,以及指挥手看到的哨兵)。
    bool from_radar = false;
    // 雷达的特殊标识:0 否 1 是 2 是但目标定位模块离线
    std::uint32_t highlight = 0;
    PositionFields<Field> position;
};

// One robot's health as the roster needs it. Health keeps the full Field
// vocabulary so a missing HP stays distinguishable from a real 0, which is what
// stops a robot with no data from being drawn as dead.
struct RobotHealth {
    RobotId id;
    std::uint32_t faction = 0;
    Field<std::uint32_t> current_hp;
    // 协议不发上限血量。自机取 RobotStaticStatus;别的车取「本局见过的最高血量」和
    // 兵种最低上限中的较大值(机器人开局满血,升级加的上限会立即加到当前血量上)。
    Field<std::uint32_t> max_hp;
    Field<std::int32_t> bullets;  // 只有己方:剩余累计发弹量
};

// Event 1 在本机累计的击杀数。终端中途启动时从 0 开始数,每局清零。
struct KillTally {
    std::uint32_t red = 0;
    std::uint32_t blue = 0;
};

struct SentryCtrlOutcome {
    std::uint32_t command_id = 0;
    std::uint32_t result_code = 0;  // 0 成功,其他失败
    MonotonicMs received_at = 0;
};

// RobotPathPlanInfo 换算后的哨兵规划路径。points 是场地坐标(米),第一个点是起点。
struct PathPlan {
    std::uint32_t intention = 0;  // 1 攻击 2 防守 3 移动
    std::uint32_t sender_id = 0;
    std::vector<std::pair<double, double>> points;
    MonotonicMs received_at = 0;
};

// 收到的 MapClickInfo。字段名照协议:mode 是标记类型(1 攻击 2 防御 3 警戒 4 自定义),
// type 是标记模式(1 地图 2 对方机器人),两个名字容易弄反。
struct MapMark {
    std::uint32_t is_send_all = 0;
    std::array<std::uint32_t, 7> recipients{};
    std::uint32_t mode = 0;
    std::uint32_t enemy_id = 0;
    std::uint32_t ascii = 0;
    std::uint32_t type = 0;
    double x = 0;
    double y = 0;
    MonotonicMs received_at = 0;
};

struct Snapshot {
    // 开机选定的席位。阵营由它决定,不靠任何消息猜。
    std::optional<RobotId> self;
    GameFields<Field> game;
    EventHistory events;
    BlindFields<Field> blind;
    // Sorted by ascending id, so the drawing order is stable across frames.
    std::vector<MapRobot> map_robots;
    std::size_t map_invalid_entries = 0;
    // Sorted by ascending id and replaced wholesale on every GlobalUnitStatus.
    std::vector<RobotHealth> robot_health;
    std::map<RobotId, RobotState> robots;
    KillTally kills;
    TechCoreFields<Field> tech_core;
    DartFields<Field> dart;
    SentryFields<Field> sentry;
    std::optional<SentryCtrlOutcome> sentry_result;
    std::optional<PathPlan> sentry_path;
    // 最近收到的地图标记,新的在后,有上限。
    std::vector<MapMark> map_marks;
};

}
