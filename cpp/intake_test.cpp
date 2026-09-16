#include "decoder.h"
#include "rm_terminal.pb.h"
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;
void require(bool value, const char* name) { if (!value) throw std::runtime_error(std::string("intake contract failed: ") + name); }
int main() {
    // Given a simulator identity and empty store; When six wire families arrive;
    // Then exact values are present under that identity (including explicit zero).
    Store store(500);
    Decoder decoder(store, RobotId{3});
    rm::GameStatus game; game.set_current_stage(4); game.set_red_score(0);
    game.set_red_economy(900); game.set_blue_economy(750);
    game.set_red_total_damage(4000); game.set_blue_total_damage(0);
    game.set_red_fortress_sec(42); game.set_fortress_holder(1);
    rm::RobotDynamicStatus dynamic; dynamic.set_robot_id(3); dynamic.set_coin(220);
    rm::RobotModuleStatus modules; modules.set_laser_detection_module(2);
    rm::RobotPosition position; position.set_x(14.5);
    rm::Event event; event.set_timestamp_ms(100); event.set_text("fixture");
    rm::RobotTelemetry telemetry; telemetry.set_timestamp_us(100); telemetry.set_bbox_h(150);
    require(decoder.accept("GameStatus", game.SerializeAsString(), 1), "GameStatus");
    require(decoder.accept("RobotDynamicStatus", dynamic.SerializeAsString(), 2), "Dynamic");
    require(decoder.accept("RobotModuleStatus", modules.SerializeAsString(), 3), "Module");
    require(decoder.accept("RobotPosition", position.SerializeAsString(), 4), "Position");
    require(decoder.accept("Event", event.SerializeAsString(), 5), "Event");
    require(decoder.accept("RobotTelemetry", telemetry.SerializeAsString(), 6), "Telemetry");
    auto s = store.snapshot(6);
    require(s.game.current_stage.value == 4 && s.game.red_score.value == 0, "Game values");
    // 每个 GameStatus 字段都要在这里断言一次:漏掉 conversion.cpp 的 MERGE 不会
    // 报错,字段会停在 NeverReceived,面板显示 "--" 而协议其实给了值。
    require(s.game.red_economy.value == 900 && s.game.blue_economy.value == 750, "Economy");
    require(s.game.red_total_damage.value == 4000, "Damage");
    require(s.game.blue_total_damage.value == 0 &&
            s.game.blue_total_damage.quality == Quality::Valid, "Zero damage is a value");
    require(s.game.red_fortress_sec.value == 42, "Fortress seconds");
    require(s.game.fortress_holder.value == 1, "Fortress holder");
    require(s.game.blue_fortress_sec.freshness == Freshness::NeverReceived,
            "Unsent field stays never-received");
    require(s.robots.at(RobotId{3}).dynamic.coin.value == 220, "Dynamic values");
    require(s.robots.at(RobotId{3}).modules.laser_detection_module.value == 2, "Module values");
    require(s.robots.at(RobotId{3}).position.x.value == 14.5, "Position values");
    require(s.event.text.value == "fixture", "Event values");
    require(s.robots.at(RobotId{3}).telemetry.bbox_h.value == 150, "Telemetry values");
    // Given existing values; When malformed, unknown, duplicate or old input arrives;
    // Then reject without refreshing the previous value.
    require(!decoder.accept("Event", event.SerializeAsString(), 7), "duplicate");
    event.set_timestamp_ms(99); require(!decoder.accept("Event", event.SerializeAsString(), 8), "stale event");
    require(!decoder.accept("GameStatus", "\xff", 9), "malformed");
    require(!decoder.accept("CustomControl", "", 10), "unknown topic");
    require(!decoder.accept("RobotPosition", position.SerializeAsString(), 0), "backwards time");
    dynamic.set_robot_id(4); require(!decoder.accept("RobotDynamicStatus", dynamic.SerializeAsString(), 11), "wrong source");
    position.set_x(std::numeric_limits<float>::quiet_NaN());
    require(!decoder.accept("RobotPosition", position.SerializeAsString(), 12), "invalid field");
    s = store.snapshot(600);
    require(s.robots.at(RobotId{3}).position.x.value == 14.5, "preserved value");
    require(s.robots.at(RobotId{3}).position.x.quality == Quality::Invalid, "invalid quality");
    require(s.robots.at(RobotId{3}).position.x.freshness == Freshness::Stale, "stale freshness");
    require(s.robots.size() == 1, "single source");
    // Given the two simulated families; When they arrive over the real decoder;
    // Then values land in the snapshot with presence preserved.
    rm::BlindStatus blind; blind.set_self_base_blinded(true); blind.set_blind_started_ms(9000); blind.set_cause(1);
    require(decoder.accept("BlindStatus", blind.SerializeAsString(), 13), "BlindStatus");
    rm::RobotPositionSet set;
    auto* teammate = set.add_entries(); teammate->set_robot_id(7); teammate->set_faction(1); teammate->set_x(6.5); teammate->set_y(2.5);
    auto* mine = set.add_entries(); mine->set_robot_id(3); mine->set_faction(1); mine->set_x(99.0); mine->set_y(99.0); mine->set_is_self(true);
    require(decoder.accept("RobotPositionSet", set.SerializeAsString(), 14), "RobotPositionSet");
    s = store.snapshot(14);
    require(s.blind.self_base_blinded.value == true, "blind flag decoded");
    require(s.blind.cause.value == 1, "blind cause decoded");
    require(s.blind.blind_remaining_ms.quality == Quality::Missing, "absent blind field stays missing");
    require(s.map_robots.size() == 2, "both map entries decoded");
    require(s.map_robots[0].id.value == 3 && s.map_robots[1].id.value == 7, "map entries sorted by id");
    require(s.map_robots[0].is_self, "the self entry is identified");
    // 14.5 is what the authoritative RobotPosition path wrote above, so the
    // fabricated 99 in the set must not survive even through the real decoder.
    require(s.map_robots[0].position.x.value == 14.5, "self position keeps the authoritative value");
    require(s.map_robots[1].position.x.value == 6.5, "teammate position comes from the set");
    // Given malformed or non-finite input on the new topics; When decoded;
    // Then reject without crashing, matching the existing invalid path.
    require(!decoder.accept("BlindStatus", "\xff", 15), "malformed BlindStatus");
    require(!decoder.accept("RobotPositionSet", "garbage", 16), "malformed RobotPositionSet");
    rm::BlindStatus bad_cause; bad_cause.set_cause(7);
    require(!decoder.accept("BlindStatus", bad_cause.SerializeAsString(), 17), "out of range blind cause");
    rm::RobotPositionSet nan_set;
    auto* broken = nan_set.add_entries(); broken->set_robot_id(5);
    broken->set_x(std::numeric_limits<float>::quiet_NaN());
    require(!decoder.accept("RobotPositionSet", nan_set.SerializeAsString(), 18), "non-finite map coordinate");
    require(store.snapshot(18).map_robots.size() == 2, "a rejected set leaves the previous map intact");
    std::cout << "eight-family decoder contract PASS\n";
}
