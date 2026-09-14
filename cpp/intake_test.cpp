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
    std::cout << "six-family decoder contract PASS\n";
}
