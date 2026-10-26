#include "config.h"
#include "match_alerts.h"

#include <cstdio>

namespace {

using rm_terminal::AlertKind;
using rm_terminal::Freshness;
using rm_terminal::MatchAlerts;
using rm_terminal::Snapshot;

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) return;
    std::fprintf(stderr, "FAIL %s\n", label);
    ++failures;
}

template <class T>
void set(rm_terminal::Field<T>& field, T value) {
    field.value = value;
    field.quality = rm_terminal::Quality::Valid;
    field.freshness = Freshness::Fresh;
}

Snapshot in_match(std::int32_t elapsed) {
    Snapshot snapshot;
    set<std::uint32_t>(snapshot.game.current_stage, 4);
    set<std::int32_t>(snapshot.game.stage_elapsed_sec, elapsed);
    return snapshot;
}

Snapshot preparing() {
    Snapshot snapshot;
    set<std::uint32_t>(snapshot.game.current_stage, 1);
    return snapshot;
}

int count(const std::vector<rm_terminal::MatchAlert>& alerts, AlertKind kind) {
    int n = 0;
    for (const auto& alert : alerts) n += alert.kind == kind;
    return n;
}

MatchAlerts red_alerts(rm_terminal::MatchAlertSettings settings = {}) {
    settings.self_faction = 1;
    return MatchAlerts(settings);
}

void outpost_temp_once_then_full_once() {
    auto alerts = red_alerts();
    int temp = 0, full = 0;
    for (int t = 10; t < 20; ++t) {
        auto snapshot = in_match(t);
        set<std::uint32_t>(snapshot.game.red_outpost_zone, t < 12 ? 2u : 3u);
        const auto out = alerts.observe(snapshot, t * 1000);
        temp += count(out, AlertKind::OutpostTemp);
        full += count(out, AlertKind::OutpostFull);
    }
    check(temp == 1, "a held temporary control alerts exactly once");
    auto snapshot = in_match(20);
    set<std::uint32_t>(snapshot.game.red_outpost_zone, 4u);
    full += count(alerts.observe(snapshot, 20000), AlertKind::OutpostFull);
    full += count(alerts.observe(snapshot, 20250), AlertKind::OutpostFull);
    check(full == 1, "turning into full control alerts once more");
}

void control_gap_hysteresis() {
    rm_terminal::MatchAlertSettings settings;
    settings.control_gap = 30;
    auto alerts = red_alerts(settings);
    const auto at = [&](std::uint32_t mine, std::uint32_t theirs, rm_terminal::MonotonicMs now) {
        auto snapshot = in_match(100);
        set(snapshot.game.red_control_points, mine);
        set(snapshot.game.blue_control_points, theirs);
        return count(alerts.observe(snapshot, now), AlertKind::ControlBehind);
    };
    check(at(10, 39, 1) == 0, "29 behind is under the threshold");
    check(at(10, 40, 2) == 1, "30 behind alerts");
    check(at(10, 60, 3) == 0, "falling further behind does not repeat");
    check(at(10, 30, 4) == 0, "20 behind is not yet recovered");
    check(at(10, 24, 5) == 0, "14 behind re-arms silently");
    check(at(10, 41, 6) == 1, "falling behind again alerts again");
}

void timeline_fires_on_time_and_never_backfills() {
    auto alerts = red_alerts();
    alerts.observe(preparing(), 0);
    int dart = 0, energy = 0;
    for (int t = 0; t <= 95; ++t) {
        const auto out = alerts.observe(in_match(t), 1000 + t * 1000);
        dart += count(out, AlertKind::DartGate);
        energy += count(out, AlertKind::Energy);
        if (t == 0) check(count(out, AlertKind::Energy) == 1, "small energy at match start");
        if (t == 20) check(count(out, AlertKind::DartGate) == 1, "dart warning 10 s before");
        if (t == 30) check(count(out, AlertKind::DartGate) == 1, "dart gate at 0:30");
        if (t == 90) check(count(out, AlertKind::Energy) == 1, "small energy again at 1:30");
    }
    check(dart == 2 && energy == 2, "nothing else fires in the first 95 s");

    // 进程启动时比赛已经进行到 3:20:不能补报 3:00 的大能量机关。
    auto late = red_alerts();
    check(late.observe(in_match(200), 0).empty(), "joining mid-match does not backfill");
    check(count(late.observe(in_match(255), 1000), AlertKind::Energy) == 1,
          "the next point after joining still fires");
    // 时钟一下跳过很多(模拟器跳转):只报刚越过 5 秒内的点。
    auto jump = red_alerts();
    jump.observe(in_match(100), 0);
    const auto out = jump.observe(in_match(243), 1000);
    check(count(out, AlertKind::Energy) == 0 && count(out, AlertKind::DartGate) == 1,
          "a jump only reports points within the last 5 s");
}

void fortress_reinforce_and_base() {
    auto alerts = red_alerts();
    auto snapshot = in_match(60);
    set<std::uint32_t>(snapshot.game.fortress_holder, 0u);
    set<std::uint32_t>(snapshot.game.red_reinforce_chances, 0u);
    set<std::uint32_t>(snapshot.game.red_base_hp, 5000u);
    check(alerts.observe(snapshot, 0).empty(), "a quiet start says nothing");

    set<std::uint32_t>(snapshot.game.fortress_holder, 2u);
    check(count(alerts.observe(snapshot, 250), AlertKind::Fortress) == 1, "enemy took the fortress");
    check(count(alerts.observe(snapshot, 500), AlertKind::Fortress) == 0, "holding is not news");
    set<std::uint32_t>(snapshot.game.fortress_holder, 0u);
    check(count(alerts.observe(snapshot, 750), AlertKind::Fortress) == 1, "fortress vacated");

    set<std::uint32_t>(snapshot.game.red_reinforce_chances, 1u);
    check(count(alerts.observe(snapshot, 1000), AlertKind::Reinforce) == 1, "a chance appeared");
    set<std::uint32_t>(snapshot.game.red_reinforce_chances, 2u);
    check(count(alerts.observe(snapshot, 1250), AlertKind::Reinforce) == 0,
          "accumulating chances does not repeat");

    set<std::uint32_t>(snapshot.game.red_base_hp, 4900u);
    check(count(alerts.observe(snapshot, 2000), AlertKind::BaseAttacked) == 1, "base took damage");
    set<std::uint32_t>(snapshot.game.red_base_hp, 4800u);
    check(count(alerts.observe(snapshot, 5000), AlertKind::BaseAttacked) == 0,
          "more damage within 10 s stays quiet");
    set<std::uint32_t>(snapshot.game.red_base_hp, 4700u);
    check(count(alerts.observe(snapshot, 12500), AlertKind::BaseAttacked) == 1,
          "damage after the quiet window alerts again");
}

void enemy_low_hp_per_robot() {
    auto alerts = red_alerts();
    const auto with_hp = [](std::uint32_t id, std::uint32_t faction, std::uint32_t hp) {
        auto snapshot = in_match(120);
        rm_terminal::RobotHealth robot;
        robot.id = rm_terminal::RobotId{id};
        robot.faction = faction;
        set(robot.current_hp, hp);
        set<std::uint32_t>(robot.max_hp, 200u);
        snapshot.robot_health.push_back(robot);
        return snapshot;
    };
    auto out = alerts.observe(with_hp(103, 2, 40), 0);
    check(count(out, AlertKind::EnemyLowHp) == 1, "enemy at 20% alerts");
    check(!out.empty() && out[0].speech == QStringLiteral("敌方3号步兵残血"), "speech names the robot");
    check(count(alerts.observe(with_hp(103, 2, 30), 250), AlertKind::EnemyLowHp) == 0,
          "dropping further stays quiet");
    check(count(alerts.observe(with_hp(3, 1, 10), 500), AlertKind::EnemyLowHp) == 0,
          "our own low robot is not an enemy alert");
    alerts.observe(with_hp(103, 2, 0), 750);
    alerts.observe(with_hp(103, 2, 200), 1000);
    check(count(alerts.observe(with_hp(103, 2, 45), 1250), AlertKind::EnemyLowHp) == 1,
          "after dying and respawning a new low-HP episode alerts");
}

void disabled_kinds_and_phrases() {
    rm_terminal::MatchAlertSettings settings;
    settings.disabled = {QStringLiteral("energy")};
    auto alerts = red_alerts(settings);
    alerts.observe(preparing(), 0);
    check(count(alerts.observe(in_match(0), 1000), AlertKind::Energy) == 0, "energy can be switched off");

    for (const QString& name : rm_terminal::alert_kind_names()) {
        bool known = false;
        for (const auto kind : {AlertKind::OutpostTemp, AlertKind::ControlBehind, AlertKind::Fortress,
                                AlertKind::DartGate, AlertKind::Energy, AlertKind::Reinforce,
                                AlertKind::EnemyLowHp, AlertKind::BaseAttacked})
            known = known || rm_terminal::alert_kind_name(kind) == name;
        check(known, "every config kind name maps to an alert kind");
    }
    const auto phrases = rm_terminal::match_alert_phrases();
    check(phrases.contains(QStringLiteral("控制度落后了")) &&
              phrases.contains(QStringLiteral("敌方7号哨兵残血")),
          "every speech line is in the pre-synthesis list");
}

void banner_queue_orders_and_expires() {
    rm_terminal::BannerQueue queue;
    queue.push(QStringLiteral("info"), rm_terminal::AlertLevel::Info, 0);
    queue.push(QStringLiteral("critical"), rm_terminal::AlertLevel::Critical, 0);
    auto shown = queue.current(0);
    check(shown && shown->text == QStringLiteral("critical"), "critical jumps the queue");
    check(queue.current(3999)->text == QStringLiteral("critical"), "a banner stays 4 s");
    check(queue.current(4000)->text == QStringLiteral("info"), "then the next one shows");
    queue.push(QStringLiteral("old"), rm_terminal::AlertLevel::Info, 4000);
    check(!queue.current(20000).has_value(), "stale banners are dropped, not shown late");
}

}  // namespace

int main() {
    outpost_temp_once_then_full_once();
    control_gap_hysteresis();
    timeline_fires_on_time_and_never_backfills();
    fortress_reinforce_and_base();
    enemy_low_hp_per_robot();
    disabled_kinds_and_phrases();
    banner_queue_orders_and_expires();
    if (failures != 0) {
        std::fprintf(stderr, "match_alerts_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("match_alerts_test: all checks passed\n");
    return 0;
}
