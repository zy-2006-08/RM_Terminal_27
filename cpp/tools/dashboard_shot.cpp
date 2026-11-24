// Screenshot harness: renders the info page with a realistic mid-match snapshot so
// the layout can be judged by eye. Not a test; not linked into the app.
#include "dashboard.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPixmap>
#include <cstdio>
#include <string>

using namespace rm_terminal;

namespace {

template <class T>
Field<T> present(T value, Freshness freshness = Freshness::Fresh) {
    Field<T> field;
    field.value = value;
    field.quality = Quality::Valid;
    field.freshness = freshness;
    return field;
}

void add_health(Snapshot& snapshot, std::uint32_t id, std::uint32_t faction,
                std::uint32_t hp, std::uint32_t max_hp, bool has_data = true) {
    RobotHealth health;
    health.id = RobotId{id};
    health.faction = faction;
    if (has_data) {
        health.current_hp = present<std::uint32_t>(hp);
        health.max_hp = present<std::uint32_t>(max_hp);
    }
    snapshot.robot_health.push_back(health);
}

void add_map_robot(Snapshot& snapshot, std::uint32_t id, std::uint32_t faction,
                   double x, double y, double yaw, bool is_self = false) {
    MapRobot robot;
    robot.id = RobotId{id};
    robot.faction = faction;
    robot.is_self = is_self;
    robot.position.x = present<double>(x);
    robot.position.y = present<double>(y);
    robot.position.yaw = present<double>(yaw);
    snapshot.map_robots.push_back(robot);
}

Snapshot match_snapshot() {
    Snapshot snapshot;
    snapshot.game.stage_countdown_sec = present<std::int32_t>(154);
    snapshot.game.current_stage = present<std::uint32_t>(4);

    // Red 1/3/4/7 alive, red 2 has no data, blue 3 down: exercises all three
    // LiveState branches the roster is supposed to distinguish.
    add_health(snapshot, 1, 1, 320, 500);
    add_health(snapshot, 2, 1, 0, 0, false);
    add_health(snapshot, 3, 1, 412, 600);
    add_health(snapshot, 4, 1, 118, 400);
    add_health(snapshot, 7, 1, 500, 500);
    add_health(snapshot, 101, 2, 470, 500);
    add_health(snapshot, 103, 2, 0, 600);
    add_health(snapshot, 104, 2, 275, 400);
    add_health(snapshot, 107, 2, 380, 500);

    add_map_robot(snapshot, 3, 1, 6.2, 7.5, 0.6, true);
    add_map_robot(snapshot, 1, 1, 4.1, 5.0, 1.2);
    add_map_robot(snapshot, 4, 1, 8.4, 9.1, -0.4);
    add_map_robot(snapshot, 101, 2, 20.5, 8.0, 3.0);
    add_map_robot(snapshot, 104, 2, 18.2, 4.4, 2.2);

    RobotState self;
    self.dynamic.current_hp = present<std::uint32_t>(412);
    self.dynamic.max_hp = present<std::uint32_t>(600);
    self.dynamic.shooter_heat_17mm = present<std::uint32_t>(85);
    self.dynamic.shooter_heat_limit = present<std::uint32_t>(200);
    self.dynamic.bullet_speed = present<double>(29.4);
    self.dynamic.remaining_ammo = present<std::uint32_t>(187);
    self.dynamic.coin = present<std::uint32_t>(642);
    self.dynamic.chassis_power = present<double>(61.8);
    self.dynamic.buffer_energy = present<double>(48.0);
    self.telemetry.vision_online = present<bool>(true);
    self.telemetry.autoaim_enabled = present<bool>(true);
    self.telemetry.target_locked = present<bool>(true);
    self.telemetry.fire_permit = present<bool>(true);
    self.telemetry.target_id = present<std::uint32_t>(104);
    self.telemetry.target_distance = present<double>(3.42);
    self.telemetry.confidence = present<double>(0.91);
    self.modules.power_manager = present<std::uint32_t>(1);
    self.modules.video_transmission = present<std::uint32_t>(1);
    self.modules.uwb = present<std::uint32_t>(1);
    self.modules.armor = present<std::uint32_t>(1);
    self.modules.main_controller = present<std::uint32_t>(1);
    snapshot.robots.emplace(RobotId{3}, self);

    const char* lines[] = {
        "蓝方 3 号步兵被击毁",
        "我方前哨站受到攻击",
        "红方 4 号装甲板 2 命中",
        "增益点占领 · 火力增益激活",
        "蓝方基地护盾剩余 60%",
        "我方 2 号哨兵失去数据链",
        "红方 1 号补给完成",
        "飞镖发射架进入锁定",
    };
    std::uint64_t at = 1000;
    std::uint32_t level = 0;
    for (const char* line : lines) {
        snapshot.events.push(
            EventRecord{at, level % 4, std::string(line), static_cast<MonotonicMs>(at)});
        at += 900;
        ++level;
    }
    snapshot.event.timestamp_ms = present<std::uint64_t>(at);
    snapshot.event.level = present<std::uint32_t>(2);
    snapshot.event.text = present<std::string>(std::string("飞镖发射架进入锁定"));

    snapshot.blind.self_base_blinded = present<bool>(false);
    return snapshot;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("/tmp/dashboard_info.png");
    const int width = argc > 3 ? QString::fromLocal8Bit(argv[2]).toInt() : 1280;
    const int height = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 720;

    Dashboard dashboard(Config{});
    dashboard.resize(width, height);

    Snapshot snapshot = match_snapshot();
    dashboard.update(snapshot, nullptr, 1000);
    dashboard.show();
    QCoreApplication::processEvents();
    for (int tick : {1250, 2500, 4300, 4550, 6000}) {
        dashboard.update(snapshot, nullptr, tick);
        QCoreApplication::processEvents();
    }

    QPixmap shot = dashboard.grab();
    if (!shot.save(path)) {
        std::fprintf(stderr, "failed to save %s\n", qPrintable(path));
        return 1;
    }
    std::printf("saved %s (%dx%d, dashboard %dx%d, mode=%d)\n", qPrintable(path),
                shot.width(), shot.height(), dashboard.width(), dashboard.height(),
                static_cast<int>(dashboard.mode()));
    return 0;
}
