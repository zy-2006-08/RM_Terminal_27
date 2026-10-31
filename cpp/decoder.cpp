#include "decoder.h"
#include "rm_terminal.pb.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace rm_terminal {
namespace {

// 规则手册 5.6:基地血量 10000,前哨站血量 1000。协议只发当前血量。
constexpr std::uint32_t kBaseMaxHp = 10000;
constexpr std::uint32_t kOutpostMaxHp = 1000;
constexpr std::uint32_t kUnknownCode = 255;  // 认不出的编码,交给转换层按无效拒绝

bool finite(double value) { return std::isfinite(value); }

std::uint32_t robot_id_of(std::uint32_t number, std::uint32_t faction) {
    return faction == kBlue ? number + 100 : number;
}

// 前哨阵地相对编码(1 己方完全 2 己方临时 3 对方临时 4 对方完全)-> 绝对编码。
std::uint32_t outpost_zone(std::uint32_t code, bool red) {
    switch (code) {
    case 0: return 0;
    case 1: return red ? 2 : 4;
    case 2: return red ? 1 : 3;
    case 3: return red ? 3 : 1;
    case 4: return red ? 4 : 2;
    default: return kUnknownCode;
    }
}
// 己方基地阵地(1 己方完全控制 2 对方临时控制)-> 绝对编码。
std::uint32_t own_base_zone(std::uint32_t code, bool red) {
    switch (code) {
    case 0: return 0;
    case 1: return red ? 2 : 4;
    case 2: return red ? 3 : 1;
    default: return kUnknownCode;
    }
}
// 对方基地阵地(1 对方完全控制 2 己方临时控制)-> 绝对编码。
std::uint32_t enemy_base_zone(std::uint32_t code, bool red) {
    switch (code) {
    case 0: return 0;
    case 1: return red ? 4 : 2;
    case 2: return red ? 1 : 3;
    default: return kUnknownCode;
    }
}

using RadarTriple = std::array<std::uint32_t, 3>;

// RadarInfoToClient 字段 1 的两种可能编码(官方原文缺字段号,见 proto 注释)都认:
// 嵌套的 RadarSingleRobotInfo,或者 x、y、标识三个一组的扁平 uint32。扁平编码当嵌套解析时,
// 第三个数(标识 0–2)会被读成字段号 0,必然失败或带出未知字段,所以先试嵌套不会误判。
std::optional<std::vector<RadarTriple>> radar_triples(const std::string& bytes) {
    rm::RadarInfoToClient nested;
    if (nested.ParseFromString(bytes)) {
        bool clean = nested.GetReflection()->GetUnknownFields(nested).empty();
        for (const auto& robot : nested.robots())
            if (!robot.GetReflection()->GetUnknownFields(robot).empty()) clean = false;
        if (clean) {
            std::vector<RadarTriple> triples;
            for (const auto& robot : nested.robots())
                triples.push_back({robot.target_pos_x(), robot.target_pos_y(), robot.is_high_light()});
            return triples;
        }
    }
    namespace pb = google::protobuf;
    pb::io::CodedInputStream input(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                   static_cast<int>(bytes.size()));
    std::vector<std::uint32_t> values;
    for (std::uint32_t tag = input.ReadTag(); tag != 0; tag = input.ReadTag()) {
        const auto field = pb::internal::WireFormatLite::GetTagFieldNumber(tag);
        const auto wire = pb::internal::WireFormatLite::GetTagWireType(tag);
        if (field == 1 && wire == pb::internal::WireFormatLite::WIRETYPE_LENGTH_DELIMITED) {
            std::uint32_t length = 0;
            if (!input.ReadVarint32(&length)) return std::nullopt;
            const auto limit = input.PushLimit(static_cast<int>(length));
            while (input.BytesUntilLimit() > 0) {
                std::uint32_t value = 0;
                if (!input.ReadVarint32(&value)) return std::nullopt;
                values.push_back(value);
            }
            input.PopLimit(limit);
        } else if (field == 1 && wire == pb::internal::WireFormatLite::WIRETYPE_VARINT) {
            std::uint32_t value = 0;
            if (!input.ReadVarint32(&value)) return std::nullopt;
            values.push_back(value);
        } else if (!pb::internal::WireFormatLite::SkipField(&input, tag)) {
            return std::nullopt;
        }
    }
    if (!input.ConsumedEntireMessage() || values.size() % 3 != 0) return std::nullopt;
    std::vector<RadarTriple> triples;
    for (std::size_t i = 0; i + 2 < values.size(); i += 3)
        triples.push_back({values[i], values[i + 1], values[i + 2]});
    return triples;
}

template<class Message>
bool parse(Message& message, const std::string& bytes) {
    return message.ParseFromString(bytes);
}

}  // namespace

const std::vector<std::string>& Decoder::topics() {
    static const std::vector<std::string> names{
        "GameStatus", "GlobalUnitStatus", "GlobalLogisticsStatus", "GlobalSpecialMechanism",
        "Event", "RobotStaticStatus", "RobotDynamicStatus", "RobotModuleStatus", "RobotPosition",
        "RobotRespawnStatus", "RadarInfoToClient", "RuneStatusSync", "TechCoreMotionStateSync",
        "DartSelectTargetStatusSync", "SentryStatusSync", "SentryCtrlResult", "RobotPathPlanInfo",
        "MapClickInfo", "RobotPerformanceSelectionSync"};
    return names;
}

bool Decoder::accept(const std::string& topic, const std::string& bytes, MonotonicMs now) {
    const std::uint32_t own = faction_of(self_.value);
    const std::uint32_t enemy = opponent_of(own);
    const bool red = own == kRed;

    if (topic == "GameStatus") {
        rm::GameStatus x;
        if (!parse(x, bytes)) return false;
        inbound::GameStatus p;
        if (x.has_current_round()) p.current_round = x.current_round();
        if (x.has_total_rounds()) p.total_rounds = x.total_rounds();
        if (x.has_red_score()) p.red_score = x.red_score();
        if (x.has_blue_score()) p.blue_score = x.blue_score();
        if (x.has_current_stage()) p.current_stage = x.current_stage();
        if (x.has_stage_countdown_sec()) p.stage_countdown_sec = x.stage_countdown_sec();
        if (x.has_stage_elapsed_sec()) p.stage_elapsed_sec = x.stage_elapsed_sec();
        if (x.has_is_paused()) p.is_paused = x.is_paused();
        if (x.has_game_result()) p.winner = x.game_result();
        if (x.has_end_reason()) p.end_reason = x.end_reason();
        return store_.apply(p, now);
    }
    if (topic == "GlobalUnitStatus") {
        rm::GlobalUnitStatus x;
        if (!parse(x, bytes)) return false;
        inbound::GameStatus p;
        if (x.has_base_health()) {
            (red ? p.red_base_hp : p.blue_base_hp) = x.base_health();
            (red ? p.red_base_max_hp : p.blue_base_max_hp) = kBaseMaxHp;
        }
        if (x.has_enemy_base_health()) {
            (red ? p.blue_base_hp : p.red_base_hp) = x.enemy_base_health();
            (red ? p.blue_base_max_hp : p.red_base_max_hp) = kBaseMaxHp;
        }
        if (x.has_base_status()) (red ? p.red_base_status : p.blue_base_status) = x.base_status();
        if (x.has_enemy_base_status())
            (red ? p.blue_base_status : p.red_base_status) = x.enemy_base_status();
        if (x.has_base_shield())
            (red ? p.red_base_zone : p.blue_base_zone) = own_base_zone(x.base_shield(), red);
        if (x.has_enemy_base_shield())
            (red ? p.blue_base_zone : p.red_base_zone) = enemy_base_zone(x.enemy_base_shield(), red);
        // R/B 前哨站的血量是绝对的(R = 开局时红方一侧),控制状态是相对的。
        if (x.has_outpost_r_health()) {
            p.red_outpost_hp = x.outpost_r_health();
            p.red_outpost_max_hp = kOutpostMaxHp;
        }
        if (x.has_outpost_b_health()) {
            p.blue_outpost_hp = x.outpost_b_health();
            p.blue_outpost_max_hp = kOutpostMaxHp;
        }
        if (x.has_outpost_r_status()) p.red_outpost_zone = outpost_zone(x.outpost_r_status(), red);
        if (x.has_outpost_b_status()) p.blue_outpost_zone = outpost_zone(x.outpost_b_status(), red);
        if (x.has_total_damage_ally())
            (red ? p.red_total_damage : p.blue_total_damage) = x.total_damage_ally();
        if (x.has_total_damage_enemy())
            (red ? p.blue_total_damage : p.red_total_damage) = x.total_damage_enemy();
        if (x.has_total_cp_ally())
            (red ? p.red_control_points : p.blue_control_points) = x.total_cp_ally();
        if (x.has_total_cp_enemy())
            (red ? p.blue_control_points : p.red_control_points) = x.total_cp_enemy();
        bool ok = store_.apply(p, now);

        // robot_health:己方 1,3,4,7,对方 1,3,4,7;robot_bullets 只有己方,协议没写顺序,按 1,3,4,7。
        if (x.robot_health_size() > 0 || x.robot_bullets_size() > 0) {
            inbound::RobotHealthSet health;
            const int slots = std::max(std::min(x.robot_health_size(), 8),
                                       std::min(x.robot_bullets_size(), 4));
            for (int i = 0; i < slots; ++i) {
                inbound::RobotHealthEntry entry;
                entry.robot_id = robot_id_of(kFieldRobots[static_cast<std::size_t>(i % 4)],
                                             i < 4 ? own : enemy);
                if (i < x.robot_health_size()) entry.current_hp = x.robot_health(i);
                if (i < 4 && i < x.robot_bullets_size()) entry.bullets = x.robot_bullets(i);
                health.entries.push_back(entry);
            }
            ok = store_.apply(health, now) && ok;
            if (x.robot_health_size() > 8 || x.robot_bullets_size() > 4) ok = false;
        }
        return ok;
    }
    if (topic == "GlobalLogisticsStatus") {
        rm::GlobalLogisticsStatus x;
        if (!parse(x, bytes)) return false;
        inbound::GameStatus p;
        if (x.has_remaining_economy()) p.economy = x.remaining_economy();
        if (x.has_total_economy_obtained()) p.total_economy = x.total_economy_obtained();
        if (x.has_tech_level()) p.tech_level = x.tech_level();
        if (x.has_encryption_level()) p.encryption_level = x.encryption_level();
        return store_.apply(p, now);
    }
    if (topic == "GlobalSpecialMechanism") {
        rm::GlobalSpecialMechanism x;
        if (!parse(x, bytes)) return false;
        // 只列正在生效的机制,所以每一帧都把六项写满:没列出的就是 0。
        inbound::GameStatus p;
        p.red_fortress_activation_sec = 0;
        p.blue_fortress_activation_sec = 0;
        p.red_outpost_zone_progress = 0;
        p.blue_outpost_zone_progress = 0;
        p.red_base_zone_progress = 0;
        p.blue_base_zone_progress = 0;
        bool ok = x.mechanism_id_size() == x.mechanism_time_sec_size();
        const int count = std::min(x.mechanism_id_size(), x.mechanism_time_sec_size());
        for (int i = 0; i < count; ++i) {
            const std::int32_t sec = x.mechanism_time_sec(i);
            // 3–6 的符号:>0 己方在推进。换成 >0 红方在推进。
            const std::int32_t toward_red = red ? sec : -sec;
            switch (x.mechanism_id(i)) {
            case 1: (red ? p.red_fortress_activation_sec : p.blue_fortress_activation_sec) = sec; break;
            case 2: (red ? p.blue_fortress_activation_sec : p.red_fortress_activation_sec) = sec; break;
            case 3: p.red_outpost_zone_progress = toward_red; break;
            case 4: p.blue_outpost_zone_progress = toward_red; break;
            case 5: p.red_base_zone_progress = toward_red; break;
            case 6: p.blue_base_zone_progress = toward_red; break;
            default: ok = false; break;
            }
        }
        return store_.apply(p, now) && ok;
    }
    if (topic == "RuneStatusSync") {
        rm::RuneStatusSync x;
        if (!parse(x, bytes)) return false;
        inbound::GameStatus p;
        if (x.has_rune_status()) p.rune_status = x.rune_status();
        if (x.has_activated_arms()) p.rune_activated_arms = x.activated_arms();
        if (x.has_average_rings()) {
            if (!finite(x.average_rings())) return false;
            p.rune_average_rings = x.average_rings();
        }
        return store_.apply(p, now);
    }
    if (topic == "RobotPerformanceSelectionSync") {
        rm::RobotPerformanceSelectionSync x;
        if (!parse(x, bytes)) return false;
        inbound::GameStatus p;
        if (x.has_shooter()) p.selected_shooter = x.shooter();
        if (x.has_chassis()) p.selected_chassis = x.chassis();
        return store_.apply(p, now);
    }
    if (topic == "Event") {
        rm::Event x;
        if (!parse(x, bytes) || !x.has_event_id()) return false;
        inbound::Event p;
        p.event_id = x.event_id();
        p.param = x.param();
        return store_.apply(p, now);
    }
    if (topic == "RobotStaticStatus") {
        rm::RobotStaticStatus x;
        if (!parse(x, bytes)) return false;
        inbound::RobotStaticStatus p;
        if (x.has_connection_state()) p.connection_state = x.connection_state();
        if (x.has_field_state()) p.field_state = x.field_state();
        if (x.has_alive_state()) p.alive_state = x.alive_state();
        if (x.has_robot_id()) p.robot_id = x.robot_id();
        if (x.has_robot_type()) p.robot_type = x.robot_type();
        if (x.has_performance_system_shooter()) p.shooter_system = x.performance_system_shooter();
        if (x.has_performance_system_chassis()) p.chassis_system = x.performance_system_chassis();
        if (x.has_level()) p.level = x.level();
        if (x.has_max_health()) p.max_hp = x.max_health();
        if (x.has_max_heat()) p.max_heat = x.max_heat();
        if (x.has_heat_cooldown_rate()) {
            if (!finite(x.heat_cooldown_rate())) return false;
            p.heat_cooldown_rate = x.heat_cooldown_rate();
        }
        if (x.has_max_power()) p.max_power = x.max_power();
        if (x.has_max_buffer_energy()) p.max_buffer_energy = x.max_buffer_energy();
        if (x.has_max_chassis_energy()) p.max_chassis_energy = x.max_chassis_energy();
        return store_.apply(p, self_, now);
    }
    if (topic == "RobotDynamicStatus") {
        rm::RobotDynamicStatus x;
        if (!parse(x, bytes)) return false;
        inbound::RobotDynamicStatus p;
        if (x.has_current_health()) p.current_hp = x.current_health();
        if (x.has_current_heat()) {
            if (!finite(x.current_heat())) return false;
            p.heat = x.current_heat();
        }
        if (x.has_last_projectile_fire_rate()) {
            if (!finite(x.last_projectile_fire_rate())) return false;
            p.bullet_speed = x.last_projectile_fire_rate();
        }
        if (x.has_current_buffer_energy()) p.buffer_energy = x.current_buffer_energy();
        if (x.has_current_experience()) p.experience = x.current_experience();
        if (x.has_experience_for_upgrade()) p.experience_for_upgrade = x.experience_for_upgrade();
        if (x.has_total_projectiles_fired()) p.total_fired = x.total_projectiles_fired();
        if (x.has_remaining_ammo()) p.remaining_ammo = x.remaining_ammo();
        if (x.has_is_out_of_combat()) p.out_of_combat = x.is_out_of_combat();
        if (x.has_out_of_combat_countdown()) p.out_of_combat_countdown = x.out_of_combat_countdown();
        if (x.has_can_remote_heal()) p.can_remote_heal = x.can_remote_heal();
        if (x.has_can_remote_ammo()) p.can_remote_ammo = x.can_remote_ammo();
        return store_.apply(p, self_, now);
    }
    if (topic == "RobotModuleStatus") {
        rm::RobotModuleStatus x;
        if (!parse(x, bytes)) return false;
        inbound::RobotModuleStatus p;
        if (x.has_power_manager()) p.power_manager = x.power_manager();
        if (x.has_rfid()) p.rfid = x.rfid();
        if (x.has_light_strip()) p.light_strip = x.light_strip();
        if (x.has_small_shooter()) p.small_shooter = x.small_shooter();
        if (x.has_big_shooter()) p.big_shooter = x.big_shooter();
        if (x.has_uwb()) p.uwb = x.uwb();
        if (x.has_armor()) p.armor = x.armor();
        if (x.has_video_transmission()) p.video_transmission = x.video_transmission();
        if (x.has_capacitor()) p.capacitor = x.capacitor();
        if (x.has_main_controller()) p.main_controller = x.main_controller();
        if (x.has_modular_42mm()) p.modular_42mm = x.modular_42mm();
        return store_.apply(p, self_, now);
    }
    if (topic == "RobotPosition") {
        rm::RobotPosition x;
        if (!parse(x, bytes)) return false;
        // 归属:没填或填自机 = 自机;指挥手还会收到己方哨兵(7/107)。别的编号不认。
        RobotId owner = self_;
        if (x.has_robot_id() && x.robot_id() != 0 && x.robot_id() != self_.value) {
            const std::uint32_t id = x.robot_id();
            const bool own_sentry = faction_of(id) == own && (id > 100 ? id - 100 : id) == 7;
            if (!own_sentry) return false;
            owner = RobotId{id};
        }
        inbound::RobotPosition p;
        if (x.has_x()) p.x = x.x();
        if (x.has_y()) p.y = x.y();
        if (x.has_yaw()) p.yaw = x.yaw();
        return store_.apply(p, owner, now);
    }
    if (topic == "RobotRespawnStatus") {
        rm::RobotRespawnStatus x;
        if (!parse(x, bytes)) return false;
        inbound::RobotRespawnStatus p;
        if (x.has_is_pending_respawn()) p.pending = x.is_pending_respawn();
        if (x.has_total_respawn_progress()) p.total_progress = x.total_respawn_progress();
        if (x.has_current_respawn_progress()) p.current_progress = x.current_respawn_progress();
        if (x.has_can_free_respawn()) p.can_free = x.can_free_respawn();
        if (x.has_gold_cost_for_respawn()) p.gold_cost = x.gold_cost_for_respawn();
        if (x.has_can_pay_for_respawn()) p.can_pay = x.can_pay_for_respawn();
        return store_.apply(p, self_, now);
    }
    if (topic == "RadarInfoToClient") {
        const auto triples = radar_triples(bytes);
        if (!triples) return false;
        if (triples->empty()) return true;
        // 顺序:对方 1,3,4,7,己方 1,3,4,7;单位 cm。0,0 视为这一帧没看到。
        inbound::RadarSet set;
        const std::size_t count = std::min<std::size_t>(triples->size(), 8);
        for (std::size_t i = 0; i < count; ++i) {
            const RadarTriple& t = (*triples)[i];
            inbound::RadarEntry entry;
            entry.robot_id = robot_id_of(kFieldRobots[i % 4], i < 4 ? enemy : own);
            if (t[0] != 0 || t[1] != 0) {
                entry.x = t[0] / 100.0;
                entry.y = t[1] / 100.0;
            }
            entry.highlight = t[2];
            set.entries.push_back(entry);
        }
        return store_.apply(set, now) && triples->size() == 8;
    }
    if (topic == "TechCoreMotionStateSync") {
        rm::TechCoreMotionStateSync x;
        if (!parse(x, bytes)) return false;
        inbound::TechCoreStatus p;
        if (x.has_maximum_difficulty_level()) p.maximum_difficulty = x.maximum_difficulty_level();
        if (x.has_basic_state()) p.basic_state = x.basic_state();
        if (x.has_putin_state()) p.putin_state = x.putin_state();
        if (x.has_move_state()) p.move_state = x.move_state();
        if (x.has_rotate_state()) p.rotate_state = x.rotate_state();
        if (x.has_enemy_core_status()) p.enemy_core_status = x.enemy_core_status();
        if (x.has_remain_time_all()) p.remain_time_all = x.remain_time_all();
        if (x.has_remain_time_step()) p.remain_time_step = x.remain_time_step();
        return store_.apply(p, now);
    }
    if (topic == "DartSelectTargetStatusSync") {
        rm::DartSelectTargetStatusSync x;
        if (!parse(x, bytes)) return false;
        inbound::DartStatus p;
        if (x.has_target_id()) p.target_id = x.target_id();
        if (x.has_open()) p.gate = x.open();
        return store_.apply(p, now);
    }
    if (topic == "SentryStatusSync") {
        rm::SentryStatusSync x;
        if (!parse(x, bytes)) return false;
        inbound::SentryStatus p;
        if (x.has_posture_id()) p.posture = x.posture_id();
        if (x.has_is_weakened()) p.weakened = x.is_weakened();
        if (x.has_is_powered()) p.powered = x.is_powered();
        return store_.apply(p, now);
    }
    if (topic == "SentryCtrlResult") {
        rm::SentryCtrlResult x;
        if (!parse(x, bytes) || !x.has_command_id()) return false;
        inbound::SentryCtrlResult p;
        p.command_id = x.command_id();
        p.result_code = x.result_code();
        return store_.apply(p, now);
    }
    if (topic == "RobotPathPlanInfo") {
        rm::RobotPathPlanInfo x;
        if (!parse(x, bytes)) return false;
        // 起点和增量的单位都是分米;增量相对上一个点(与串口 0x0307 的定义一致)。
        inbound::PathPlan p;
        p.intention = x.intention();
        p.sender_id = x.sender_id();
        double px = x.start_pos_x() / 10.0;
        double py = x.start_pos_y() / 10.0;
        p.points.emplace_back(px, py);
        const int steps = std::min(x.offset_x_size(), x.offset_y_size());
        for (int i = 0; i < steps; ++i) {
            px += x.offset_x(i) / 10.0;
            py += x.offset_y(i) / 10.0;
            p.points.emplace_back(px, py);
        }
        return store_.apply(p, now) && x.offset_x_size() == x.offset_y_size();
    }
    if (topic == "MapClickInfo") {
        rm::MapClickInfo x;
        if (!parse(x, bytes)) return false;
        if (!finite(x.map_x()) || !finite(x.map_y())) return false;
        inbound::MapClick p;
        p.is_send_all = x.is_send_all();
        const std::string& ids = x.robot_id();
        for (std::size_t i = 0; i < p.recipients.size() && i < ids.size(); ++i)
            p.recipients[i] = static_cast<std::uint8_t>(ids[i]);
        p.mode = x.mode();
        p.enemy_id = x.enemy_id();
        p.ascii = x.ascii();
        p.type = x.type();
        p.x = x.map_x();
        p.y = x.map_y();
        return store_.apply(p, now);
    }
    return false;
}
}
