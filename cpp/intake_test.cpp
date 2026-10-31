#include "decoder.h"
#include "rm_terminal.pb.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace rm_terminal;
void require(bool value, const char* name) {
    if (!value) throw std::runtime_error(std::string("intake contract failed: ") + name);
}

namespace {

const RobotHealth* health_of(const Snapshot& s, std::uint32_t id) {
    for (const auto& robot : s.robot_health)
        if (robot.id.value == id) return &robot;
    return nullptr;
}
const MapRobot* map_of(const Snapshot& s, std::uint32_t id) {
    for (const auto& robot : s.map_robots)
        if (robot.id.value == id) return &robot;
    return nullptr;
}

rm::GlobalUnitStatus unit_status() {
    rm::GlobalUnitStatus x;
    x.set_base_health(8000);
    x.set_base_status(1);
    x.set_base_shield(2);         // 己方基地阵地被对方临时控制
    x.set_outpost_r_health(600);
    x.set_outpost_r_status(3);    // R 前哨阵地被对方临时控制
    x.set_enemy_base_health(9000);
    x.set_enemy_base_status(0);
    x.set_enemy_base_shield(1);   // 对方基地阵地被对方完全控制
    x.set_outpost_b_health(0);
    x.set_outpost_b_status(2);    // B 前哨阵地被己方临时控制
    for (std::uint32_t hp : {400u, 200u, 0u, 400u, 450u, 150u, 175u, 350u}) x.add_robot_health(hp);
    for (std::int32_t bullets : {10, 200, 0, 300}) x.add_robot_bullets(bullets);
    x.set_total_damage_ally(1200);
    x.set_total_damage_enemy(0);
    x.set_total_cp_ally(30);
    x.set_total_cp_enemy(45);
    return x;
}

// RadarInfoToClient 字段 1 的另一种可能编码:x、y、标识三个一组的 packed uint32。
std::string flat_radar(const std::vector<std::uint32_t>& values) {
    std::string body;
    {
        google::protobuf::io::StringOutputStream stream(&body);
        google::protobuf::io::CodedOutputStream out(&stream);
        for (std::uint32_t v : values) out.WriteVarint32(v);
    }
    std::string bytes;
    {
        google::protobuf::io::StringOutputStream stream(&bytes);
        google::protobuf::io::CodedOutputStream out(&stream);
        out.WriteTag((1u << 3) | 2u);
        out.WriteVarint32(static_cast<std::uint32_t>(body.size()));
        out.WriteRaw(body.data(), static_cast<int>(body.size()));
    }
    return bytes;
}

void red_seat_maps_relative_fields_to_red_and_blue() {
    Store store(500);
    Decoder decoder(store, RobotId{3});
    rm::GameStatus game;
    game.set_current_round(1);
    game.set_total_rounds(3);
    game.set_current_stage(4);
    game.set_red_score(0);
    game.set_stage_countdown_sec(300);
    game.set_game_result(255);
    require(decoder.accept("GameStatus", game.SerializeAsString(), 1), "GameStatus");
    require(decoder.accept("GlobalUnitStatus", unit_status().SerializeAsString(), 2), "GlobalUnitStatus");
    auto s = store.snapshot(2);
    require(s.self && s.self->value == 3, "the seat reaches the snapshot");
    require(s.game.winner.value == 255u, "game_result lands in winner");
    require(s.game.red_score.value == 0u && s.game.red_score.quality == Quality::Valid, "zero score is a value");
    require(s.game.red_base_hp.value == 8000u && s.game.blue_base_hp.value == 9000u, "base hp by side");
    require(s.game.red_base_max_hp.value == 10000u && s.game.blue_base_max_hp.value == 10000u,
            "base max hp comes from the rules");
    require(s.game.red_base_status.value == 1u && s.game.blue_base_status.value == 0u, "base status by side");
    require(s.game.red_outpost_hp.value == 600u && s.game.blue_outpost_hp.value == 0u,
            "R/B outpost hp is absolute");
    require(s.game.red_outpost_max_hp.value == 1000u, "outpost max hp comes from the rules");
    require(s.game.red_outpost_zone.value == 3u, "R outpost zone: enemy temp -> blue temp");
    require(s.game.blue_outpost_zone.value == 1u, "B outpost zone: ally temp -> red temp");
    require(s.game.red_base_zone.value == 3u, "own base zone: enemy temp -> blue temp");
    require(s.game.blue_base_zone.value == 4u, "enemy base zone: enemy full -> blue full");
    require(s.game.red_total_damage.value == 1200u && s.game.blue_total_damage.value == 0u &&
                s.game.blue_total_damage.quality == Quality::Valid, "damage by side, zero kept");
    require(s.game.red_control_points.value == 30u && s.game.blue_control_points.value == 45u,
            "control points by side");
    require(s.robot_health.size() == 8, "eight robots from robot_health");
    require(health_of(s, 1)->current_hp.value == 400u && health_of(s, 7)->current_hp.value == 400u,
            "ally slots are 1,3,4,7");
    require(health_of(s, 101)->current_hp.value == 450u && health_of(s, 107)->current_hp.value == 350u,
            "enemy slots are 101,103,104,107");
    require(health_of(s, 4)->current_hp.value == 0u && health_of(s, 4)->current_hp.quality == Quality::Valid,
            "zero hp is dead, not missing");
    require(health_of(s, 3)->bullets.value == 200 && health_of(s, 7)->bullets.value == 300,
            "bullets follow the ally order");
    require(!health_of(s, 103)->bullets.value, "enemy bullets are never reported");
    require(health_of(s, 101)->max_hp.value == 450u, "max hp rises to the highest hp seen");
    require(health_of(s, 103)->max_hp.value == 150u, "max hp never falls below the class floor");
    require(health_of(s, 1)->faction == kRed && health_of(s, 101)->faction == kBlue, "faction from id");
    require(s.game.economy.freshness == Freshness::NeverReceived, "unsent logistics stay never-received");
}

void blue_seat_mirrors_the_same_message() {
    Store store(500);
    Decoder decoder(store, RobotId{103});
    require(decoder.accept("GlobalUnitStatus", unit_status().SerializeAsString(), 1), "blue GlobalUnitStatus");
    const auto s = store.snapshot(1);
    require(s.game.blue_base_hp.value == 8000u && s.game.red_base_hp.value == 9000u, "blue: own base is blue");
    require(s.game.blue_base_status.value == 1u, "blue: own base status");
    require(s.game.red_outpost_hp.value == 600u, "blue: R outpost hp stays red");
    require(s.game.red_outpost_zone.value == 1u, "blue: R outpost enemy temp -> red temp");
    require(s.game.blue_outpost_zone.value == 3u, "blue: B outpost ally temp -> blue temp");
    require(s.game.blue_base_zone.value == 1u, "blue: own base enemy temp -> red temp");
    require(s.game.red_base_zone.value == 2u, "blue: enemy base enemy full -> red full");
    require(s.game.blue_control_points.value == 30u && s.game.red_control_points.value == 45u,
            "blue: control points mirrored");
    require(health_of(s, 101)->current_hp.value == 400u && health_of(s, 1)->current_hp.value == 450u,
            "blue: ally slots are 101,103,104,107");
    require(health_of(s, 103)->bullets.value == 200, "blue: bullets on blue ids");
}

void special_mechanisms_fill_every_timer() {
    Store store(500);
    Decoder decoder(store, RobotId{3});
    rm::GlobalSpecialMechanism x;
    x.add_mechanism_id(1); x.add_mechanism_time_sec(4);
    x.add_mechanism_id(3); x.add_mechanism_time_sec(-6);
    require(decoder.accept("GlobalSpecialMechanism", x.SerializeAsString(), 1), "GlobalSpecialMechanism");
    auto s = store.snapshot(1);
    require(s.game.red_fortress_activation_sec.value == 4, "own fortress activated by the enemy");
    require(s.game.blue_fortress_activation_sec.value == 0, "unlisted fortress timer is zero");
    require(s.game.red_outpost_zone_progress.value == -6, "enemy pushing R outpost -> blue pushing");
    require(s.game.blue_base_zone_progress.value == 0 &&
                s.game.blue_base_zone_progress.quality == Quality::Valid, "unlisted progress is a real zero");
    rm::GlobalSpecialMechanism bad;
    bad.add_mechanism_id(2);
    require(!decoder.accept("GlobalSpecialMechanism", bad.SerializeAsString(), 2), "mismatched arrays flagged");

    Store blue_store(500);
    Decoder blue(blue_store, RobotId{106});
    require(blue.accept("GlobalSpecialMechanism", x.SerializeAsString(), 1), "blue mechanisms");
    s = blue_store.snapshot(1);
    require(s.game.blue_fortress_activation_sec.value == 4, "blue: own fortress is blue");
    require(s.game.red_outpost_zone_progress.value == 6, "blue: enemy (red) pushing is positive");
}

void events_count_kills_and_blind_the_operator() {
    Store store(500);
    Decoder decoder(store, RobotId{3});
    rm::GameStatus game;
    game.set_current_round(1);
    game.set_current_stage(4);
    game.set_stage_countdown_sec(250);
    require(decoder.accept("GameStatus", game.SerializeAsString(), 0), "match running");

    rm::Event kill;
    kill.set_event_id(1);
    kill.set_param("3,101");
    require(decoder.accept("Event", kill.SerializeAsString(), 10), "kill event");
    auto s = store.snapshot(10);
    require(s.kills.blue == 1 && s.kills.red == 0, "the killer's side is credited");
    const auto record = s.events.recent(1).front();
    require(record.text.find("击毁") != std::string::npos, "kill text");
    require(record.countdown_sec == 250, "event remembers the match clock");
    require(record.faction == kBlue && record.event_id == 1, "kill banner is the killer's side");

    // 对方(蓝)飞镖命中我方基地随机移动目标:遮挡 10 秒。
    rm::Event hit;
    hit.set_event_id(9);
    hit.set_param("2,4");
    require(decoder.accept("Event", hit.SerializeAsString(), 1000), "dart hit");
    s = store.snapshot(6000);
    require(s.blind.self_base_blinded.value == true, "enemy hit blinds us");
    require(s.blind.self_base_blinded.freshness == Freshness::Fresh, "an active blind is trustworthy");
    require(s.blind.blind_remaining_ms.value == 5000, "remaining time counts down");
    require(s.blind.target.value == 4u, "the hit target is kept");
    require(s.events.recent(1).front().text.find("遮挡 10 秒") != std::string::npos, "blind duration shown");

    // 连续命中叠加:剩 5 秒时再中一发固定目标(本局第 1 次,10 秒)。
    hit.set_param("2,2");
    require(decoder.accept("Event", hit.SerializeAsString(), 6000), "second hit");
    require(store.snapshot(20900).blind.self_base_blinded.value == true, "accumulated window still open");
    require(store.snapshot(21000).blind.self_base_blinded.value == false, "accumulated window closes at 5+10 s");
    // 第 2、3、4、5 次固定目标:5、3、2、2 秒。
    const MonotonicMs expected[] = {5000, 3000, 2000, 2000};
    MonotonicMs at = 30000;
    for (MonotonicMs duration : expected) {
        require(decoder.accept("Event", hit.SerializeAsString(), at), "fixed hit");
        require(store.snapshot(at).blind.blind_remaining_ms.value == duration, "rule duration for the nth fixed hit");
        at += 20000;
    }
    // 我方命中对方:不遮挡我方。
    hit.set_param("1,4");
    require(decoder.accept("Event", hit.SerializeAsString(), at), "our hit");
    require(store.snapshot(at).blind.self_base_blinded.value == false, "our own hit does not blind us");

    // 换局:击杀数和命中次数清零。
    game.set_current_round(2);
    require(decoder.accept("GameStatus", game.SerializeAsString(), at + 1), "next round");
    s = store.snapshot(at + 1);
    require(s.kills.blue == 0, "kills reset with the round");
    hit.set_param("2,1");
    require(decoder.accept("Event", hit.SerializeAsString(), at + 2), "first hit of round two");
    require(store.snapshot(at + 2).blind.blind_remaining_ms.value == 10000, "hit count resets with the round");

    rm::Event unknown;
    unknown.set_event_id(99);
    require(!decoder.accept("Event", unknown.SerializeAsString(), at + 3), "unknown event id is flagged");
    require(store.snapshot(at + 3).events.recent(1).front().text.find("99") != std::string::npos,
            "unknown event still shows up verbatim");
}

void blind_freshness_follows_the_server_stream() {
    Store store(500);
    Decoder decoder(store, RobotId{3});
    require(store.snapshot(0).blind.self_base_blinded.freshness == Freshness::NeverReceived,
            "no stream, no blind knowledge");
    rm::GameStatus game;
    game.set_current_stage(4);
    require(decoder.accept("GameStatus", game.SerializeAsString(), 0), "stream alive");
    require(store.snapshot(100).blind.self_base_blinded.freshness == Freshness::Fresh, "live stream: fresh");
    require(store.snapshot(100).blind.self_base_blinded.value == false, "live stream, no hit: clear");
    require(store.snapshot(600).blind.self_base_blinded.freshness == Freshness::Stale,
            "stream silent: a missed hit is possible");
}

void radar_positions_in_both_encodings() {
    Store store(500);
    Decoder decoder(store, RobotId{3});
    rm::RobotPosition self;
    self.set_x(14.5f);
    self.set_y(3.0f);
    self.set_yaw(90.0f);
    self.set_robot_id(3);
    require(decoder.accept("RobotPosition", self.SerializeAsString(), 1), "self position");

    rm::RadarInfoToClient radar;
    // 对方 1,3,4,7,己方 1,3,4,7,单位 cm。
    const std::uint32_t xy[8][2] = {{2000, 700}, {0, 0}, {2500, 300}, {2700, 800},
                                    {500, 400}, {9900, 9900}, {800, 1000}, {300, 700}};
    for (const auto& p : xy) {
        auto* robot = radar.add_robots();
        robot->set_target_pos_x(p[0]);
        robot->set_target_pos_y(p[1]);
        robot->set_is_high_light(0);
    }
    radar.mutable_robots(0)->set_is_high_light(1);
    require(decoder.accept("RadarInfoToClient", radar.SerializeAsString(), 2), "nested radar");
    auto s = store.snapshot(2);
    require(map_of(s, 101) && map_of(s, 101)->position.x.value == 20.0, "cm -> m");
    require(map_of(s, 101)->highlight == 1u && map_of(s, 101)->from_radar, "radar flag kept");
    require(map_of(s, 103) && !map_of(s, 103)->position.x.value, "0,0 means not seen");
    require(map_of(s, 3)->is_self && map_of(s, 3)->position.x.value == 14.5 && !map_of(s, 3)->from_radar,
            "self coordinates only come from RobotPosition");
    require(map_of(s, 7)->position.y.value == 7.0, "ally sentry from radar");

    // 扁平编码:x、y、标识三个一组。这一帧没看到对方 1 号(0,0),上一次的位置保留下来。
    std::vector<std::uint32_t> flat;
    const std::uint32_t xy2[8][2] = {{0, 0}, {1500, 750}, {2500, 300}, {2700, 800},
                                     {500, 400}, {0, 0}, {800, 1000}, {300, 700}};
    for (const auto& p : xy2) {
        flat.push_back(p[0]);
        flat.push_back(p[1]);
        flat.push_back(0);
    }
    require(decoder.accept("RadarInfoToClient", flat_radar(flat), 900), "flat radar");
    s = store.snapshot(900);
    require(map_of(s, 103)->position.x.value == 15.0, "flat encoding decodes");
    require(map_of(s, 101)->position.x.value == 20.0 && map_of(s, 101)->position.x.freshness == Freshness::Stale,
            "an unseen robot keeps its last position and ages");
    require(!decoder.accept("RadarInfoToClient", "\x0a\x02\x08", 901), "garbage radar rejected");
}

void sentry_position_only_for_own_sentry() {
    Store store(500);
    Decoder commander(store, RobotId{6});
    rm::RobotPosition sentry;
    sentry.set_x(10.0f);
    sentry.set_y(5.0f);
    sentry.set_robot_id(7);
    require(commander.accept("RobotPosition", sentry.SerializeAsString(), 1), "commander gets the sentry");
    auto s = store.snapshot(1);
    require(map_of(s, 7) && map_of(s, 7)->position.x.value == 10.0 && !map_of(s, 7)->is_self,
            "sentry drawn from the server position");
    sentry.set_robot_id(107);
    require(!commander.accept("RobotPosition", sentry.SerializeAsString(), 2), "enemy sentry rejected");
    sentry.set_robot_id(4);
    require(!commander.accept("RobotPosition", sentry.SerializeAsString(), 3), "teammate id rejected");
}

void self_robot_messages() {
    Store store(500);
    Decoder decoder(store, RobotId{1});
    rm::RobotStaticStatus statics;
    statics.set_robot_id(1);
    statics.set_max_health(600);
    statics.set_max_heat(300);
    statics.set_level(3);
    rm::RobotDynamicStatus dynamic;
    dynamic.set_current_health(420);
    dynamic.set_current_heat(85.5f);
    dynamic.set_remaining_ammo(0);
    dynamic.set_is_out_of_combat(true);
    rm::RobotModuleStatus modules;
    modules.set_modular_42mm(1);
    modules.set_armor(2);
    rm::RobotRespawnStatus respawn;
    respawn.set_is_pending_respawn(false);
    require(decoder.accept("RobotStaticStatus", statics.SerializeAsString(), 1), "static");
    require(decoder.accept("RobotDynamicStatus", dynamic.SerializeAsString(), 2), "dynamic");
    require(decoder.accept("RobotModuleStatus", modules.SerializeAsString(), 3), "modules");
    require(decoder.accept("RobotRespawnStatus", respawn.SerializeAsString(), 4), "respawn");
    rm::GlobalUnitStatus unit;
    for (std::uint32_t hp : {420u, 0u, 0u, 0u}) unit.add_robot_health(hp);
    require(decoder.accept("GlobalUnitStatus", unit.SerializeAsString(), 5), "unit");
    const auto s = store.snapshot(5);
    const RobotState& self = s.robots.at(RobotId{1});
    require(self.statics.max_hp.value == 600u && self.statics.level.value == 3u, "static values");
    require(self.dynamic.current_hp.value == 420u && self.dynamic.heat.value == 85.5, "dynamic values");
    require(self.dynamic.remaining_ammo.value == 0u, "zero ammo is a value");
    require(self.dynamic.out_of_combat.value == true, "out of combat");
    require(self.modules.modular_42mm.value == 1u && self.modules.armor.value == 2u, "module values");
    require(self.respawn.pending.value == false, "respawn values");
    require(health_of(s, 1)->max_hp.value == 600u, "own max hp comes from RobotStaticStatus");
    require(s.robots.size() == 1, "everything lands on the seat");
}

void performance_selection_sync() {
    Store store(500);
    Decoder decoder(store, RobotId{3});
    rm::RobotPerformanceSelectionSync sync;
    sync.set_shooter(2);
    sync.set_chassis(1);
    sync.set_sentry_control(1);
    require(decoder.accept("RobotPerformanceSelectionSync", sync.SerializeAsString(), 1), "performance sync");
    const auto s = store.snapshot(1);
    require(s.game.selected_shooter.value == 2u && s.game.selected_chassis.value == 1u,
            "performance selection lands in game fields");
    require(std::find(Decoder::topics().begin(), Decoder::topics().end(),
                      "RobotPerformanceSelectionSync") != Decoder::topics().end(),
            "terminal subscribes RobotPerformanceSelectionSync");
}

void commander_and_hero_messages() {
    Store store(500);
    Decoder decoder(store, RobotId{106});
    rm::DartSelectTargetStatusSync dart;
    dart.set_target_id(4);
    dart.set_open(2);
    rm::SentryStatusSync sentry;
    sentry.set_posture_id(2);
    sentry.set_is_powered(true);
    rm::SentryCtrlResult result;
    result.set_command_id(8);
    result.set_result_code(3);
    rm::RobotPathPlanInfo path;
    path.set_intention(3);
    path.set_start_pos_x(100);
    path.set_start_pos_y(50);
    path.set_sender_id(107);
    for (int i = 0; i < 49; ++i) {
        path.add_offset_x(i < 2 ? 10 : 0);
        path.add_offset_y(i < 2 ? -5 : 0);
    }
    rm::MapClickInfo mark;
    mark.set_is_send_all(1);
    mark.set_robot_id(std::string("\x65\x67\x00\x00\x00\x00\x00", 7));
    mark.set_mode(1);
    mark.set_type(1);
    mark.set_map_x(12.5f);
    mark.set_map_y(7.25f);
    rm::TechCoreMotionStateSync core;
    core.set_maximum_difficulty_level(3);
    core.set_basic_state(2);
    core.set_putin_state(1);
    require(decoder.accept("DartSelectTargetStatusSync", dart.SerializeAsString(), 1), "dart");
    require(decoder.accept("SentryStatusSync", sentry.SerializeAsString(), 2), "sentry");
    require(decoder.accept("SentryCtrlResult", result.SerializeAsString(), 3), "sentry result");
    require(decoder.accept("RobotPathPlanInfo", path.SerializeAsString(), 4), "path");
    require(decoder.accept("MapClickInfo", mark.SerializeAsString(), 5), "mark");
    require(decoder.accept("TechCoreMotionStateSync", core.SerializeAsString(), 6), "tech core");
    const auto s = store.snapshot(6);
    require(s.dart.target_id.value == 4u && s.dart.gate.value == 2u, "dart values");
    require(s.sentry.posture.value == 2u && s.sentry.powered.value == true, "sentry values");
    require(s.sentry_result && s.sentry_result->command_id == 8 && s.sentry_result->result_code == 3,
            "sentry result kept");
    require(s.sentry_path && s.sentry_path->points.size() == 50, "start + 49 path points");
    require(s.sentry_path->points[0].first == 10.0 && s.sentry_path->points[0].second == 5.0, "start in dm");
    require(s.sentry_path->points[2].first == 12.0 && s.sentry_path->points[2].second == 4.0,
            "offsets accumulate from the previous point");
    require(s.map_marks.size() == 1 && s.map_marks[0].recipients[0] == 101u &&
                s.map_marks[0].recipients[1] == 103u && s.map_marks[0].x == 12.5, "mark values");
    require(s.tech_core.maximum_difficulty.value == 3u && s.tech_core.putin_state.value == 1u, "tech core");
}

void rejects_bad_input_without_touching_state() {
    Store store(500);
    Decoder decoder(store, RobotId{3});
    rm::RobotPosition position;
    position.set_x(14.5f);
    require(decoder.accept("RobotPosition", position.SerializeAsString(), 4), "position");
    require(!decoder.accept("GameStatus", "\xff", 9), "malformed");
    require(!decoder.accept("CustomControl", "", 10), "unknown topic");
    require(!decoder.accept("BlindStatus", "", 10), "the 2026 simulator topic is gone");
    require(!decoder.accept("RobotPosition", position.SerializeAsString(), 0), "backwards time");
    position.set_x(std::numeric_limits<float>::quiet_NaN());
    require(!decoder.accept("RobotPosition", position.SerializeAsString(), 12), "invalid field");
    rm::GlobalUnitStatus zone;
    zone.set_outpost_r_status(9);
    require(!decoder.accept("GlobalUnitStatus", zone.SerializeAsString(), 13), "unknown zone code");
    const auto s = store.snapshot(600);
    require(s.robots.at(RobotId{3}).position.x.value == 14.5, "preserved value");
    require(s.robots.at(RobotId{3}).position.x.quality == Quality::Invalid, "invalid quality");
    require(s.robots.at(RobotId{3}).position.x.freshness == Freshness::Stale, "stale freshness");
    require(s.game.red_outpost_zone.quality == Quality::Invalid && !s.game.red_outpost_zone.value,
            "unknown zone code is never drawn");
}

}  // namespace

int main() {
    try {
        red_seat_maps_relative_fields_to_red_and_blue();
        blue_seat_mirrors_the_same_message();
        special_mechanisms_fill_every_timer();
        events_count_kills_and_blind_the_operator();
        blind_freshness_follows_the_server_stream();
        radar_positions_in_both_encodings();
        sentry_position_only_for_own_sentry();
        self_robot_messages();
        performance_selection_sync();
        commander_and_hero_messages();
        rejects_bad_input_without_touching_state();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "2027 decoder contract PASS\n";
    return 0;
}
