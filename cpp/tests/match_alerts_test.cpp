#include "config.h"
#include "match_alerts.h"

#include <cstdio>

namespace
{

    using rm_terminal::AlertKind;
    using rm_terminal::Freshness;
    using rm_terminal::MatchAlerts;
    using rm_terminal::Snapshot;

    int failures = 0;

    void check(bool condition, const char* label)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }

    template <class T>
    void set(rm_terminal::Field<T>& field, T value)
    {
        field.value = value;
        field.quality = rm_terminal::Quality::Valid;
        field.freshness = Freshness::Fresh;
    }

    Snapshot in_match(std::int32_t elapsed)
    {
        Snapshot snapshot;
        set<std::uint32_t>(snapshot.game.current_stage, 4);
        set<std::int32_t>(snapshot.game.stage_elapsed_sec, elapsed);
        return snapshot;
    }

    Snapshot preparing()
    {
        Snapshot snapshot;
        set<std::uint32_t>(snapshot.game.current_stage, 1);
        return snapshot;
    }

    int count(const std::vector<rm_terminal::MatchAlert>& alerts, AlertKind kind)
    {
        int n = 0;
        for (const auto& alert : alerts)
            n += alert.kind == kind;
        return n;
    }

    rm_terminal::MapRobot radar_robot(std::uint32_t id, double x, double y)
    {
        rm_terminal::MapRobot robot;
        robot.id = rm_terminal::RobotId{id};
        robot.faction = rm_terminal::faction_of(id);
        robot.from_radar = true;
        set(robot.position.x, x);
        set(robot.position.y, y);
        return robot;
    }

    MatchAlerts red_alerts(rm_terminal::MatchAlertSettings settings = {})
    {
        settings.self_faction = 1;
        return MatchAlerts(settings);
    }

    void outpost_temp_once_then_full_once()
    {
        auto alerts = red_alerts();
        int temp = 0, full = 0;
        for (int t = 10; t < 20; ++t)
        {
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

    void control_gap_hysteresis()
    {
        rm_terminal::MatchAlertSettings settings;
        settings.control_gap = 30;
        auto alerts = red_alerts(settings);
        const auto at = [&](std::uint32_t mine, std::uint32_t theirs, rm_terminal::MonotonicMs now)
        {
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

    void timeline_fires_on_time_and_never_backfills()
    {
        rm_terminal::MatchAlertSettings commander;
        commander.role = rm_terminal::Role::Commander;
        auto alerts = red_alerts(commander);
        alerts.observe(preparing(), 0);
        int dart = 0, energy = 0;
        for (int t = 0; t <= 95; ++t)
        {
            const auto out = alerts.observe(in_match(t), 1000 + t * 1000);
            dart += count(out, AlertKind::DartGate);
            energy += count(out, AlertKind::Energy);
            if (t == 0)
                check(count(out, AlertKind::Energy) == 1, "small energy at match start");
            if (t == 20)
                check(count(out, AlertKind::DartGate) == 1, "dart warning 10 s before");
            if (t == 30)
                check(count(out, AlertKind::DartGate) == 1, "dart gate at 0:30");
            if (t == 90)
                check(count(out, AlertKind::Energy) == 1, "small energy again at 1:30");
        }
        check(dart == 2 && energy == 2, "nothing else fires in the first 95 s");

        // 进程启动时比赛已经进行到 3:20:不能补报 3:00 的大能量机关。
        auto late = red_alerts(commander);
        check(late.observe(in_match(200), 0).empty(), "joining mid-match does not backfill");
        check(count(late.observe(in_match(255), 1000), AlertKind::Energy) == 1,
              "the next point after joining still fires");
        // 时钟一下跳过很多(模拟器跳转):只报刚越过 5 秒内的点。
        auto jump = red_alerts(commander);
        jump.observe(in_match(100), 0);
        const auto out = jump.observe(in_match(243), 1000);
        check(count(out, AlertKind::Energy) == 0 && count(out, AlertKind::DartGate) == 1,
              "a jump only reports points within the last 5 s");
    }

    void dart_gate_quiet_once_our_darts_are_spent()
    {
        rm_terminal::MatchAlertSettings commander;
        commander.role = rm_terminal::Role::Commander;
        const auto heard = [&](std::uint32_t red_hits, std::uint32_t blue_hits)
        {
            auto alerts = red_alerts(commander);
            alerts.observe(preparing(), 0);
            int n = 0;
            for (int t = 225; t <= 245; ++t)
            {
                auto snapshot = in_match(t);
                snapshot.dart_hits = rm_terminal::DartTally{red_hits, blue_hits};
                n += count(alerts.observe(snapshot, 1000 + t * 1000), AlertKind::DartGate);
            }
            return n;
        };
        check(heard(3, 0) == 2, "a dart left: 3:50 and 4:00 still remind");
        check(heard(4, 0) == 0, "all four darts hit: no gate reminders");
        check(heard(0, 4) == 2, "the enemy's darts do not use up ours");
    }

    void alerts_follow_the_seat()
    {
        const auto heard = [](rm_terminal::Role role, AlertKind kind)
        {
            rm_terminal::MatchAlertSettings settings;
            settings.role = role;
            auto alerts = red_alerts(settings);
            alerts.observe(preparing(), 0);
            int n = 0;
            for (int t = 0; t <= 245; ++t)
                n += count(alerts.observe(in_match(t), 1000 + t * 1000), kind);
            return n;
        };
        using rm_terminal::Role;
        check(heard(Role::Hero, AlertKind::Energy) == 0, "the heavy cannot open the energy mechanism");
        check(heard(Role::Hero, AlertKind::DartGate) == 0, "the heavy does not run the dart");
        check(heard(Role::Infantry, AlertKind::Energy) == 3, "infantry hears 0:00, 1:30 and 3:00");
        check(heard(Role::Infantry, AlertKind::DartGate) == 0, "infantry does not run the dart");
        check(heard(Role::Commander, AlertKind::DartGate) == 4, "the commander hears both gate windows");
        check(heard(Role::Commander, AlertKind::Energy) == 3, "the commander still hears the energy windows");

        rm_terminal::MatchAlertSettings heavy;
        heavy.role = Role::Hero;
        auto alerts = red_alerts(heavy);
        auto snapshot = in_match(60);
        set<std::uint32_t>(snapshot.game.red_outpost_zone, 3u);
        check(count(alerts.observe(snapshot, 0), AlertKind::OutpostTemp) == 1, "zone alerts reach every seat");
    }

    void fortress_and_base()
    {
        auto alerts = red_alerts();
        auto snapshot = in_match(60);
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 0);
        set<std::int32_t>(snapshot.game.blue_fortress_activation_sec, 0);
        set<std::uint32_t>(snapshot.game.red_base_hp, 5000u);
        check(alerts.observe(snapshot, 0).empty(), "a quiet start says nothing");

        // 我方(红)在激活敌方堡垒不是要提醒的事。
        set<std::int32_t>(snapshot.game.blue_fortress_activation_sec, 3);
        check(count(alerts.observe(snapshot, 100), AlertKind::Fortress) == 0, "our own activation is quiet");
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 1);
        check(count(alerts.observe(snapshot, 250), AlertKind::Fortress) == 1,
              "the enemy starting on our fortress fires once");
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 5);
        check(count(alerts.observe(snapshot, 500), AlertKind::Fortress) == 0, "the timer running is not news");
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 0);
        check(count(alerts.observe(snapshot, 750), AlertKind::Fortress) == 0, "the enemy leaving is quiet");
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 2);
        check(count(alerts.observe(snapshot, 1000), AlertKind::Fortress) == 1,
              "a new activation after a break fires again");

        set<std::uint32_t>(snapshot.game.red_base_hp, 4900u);
        check(count(alerts.observe(snapshot, 2000), AlertKind::BaseAttacked) == 1, "base took damage");
        set<std::uint32_t>(snapshot.game.red_base_hp, 4800u);
        check(count(alerts.observe(snapshot, 5000), AlertKind::BaseAttacked) == 0,
              "more damage within 10 s stays quiet");
        set<std::uint32_t>(snapshot.game.red_base_hp, 4700u);
        check(count(alerts.observe(snapshot, 12500), AlertKind::BaseAttacked) == 1,
              "damage after the quiet window alerts again");
    }

    // 偷堡垒的那台 100% 易伤,我方基地护甲已经展开时就没有了(规则 5.6.4.9)。
    void fortress_thief_text()
    {
        const auto banner_of = [](const std::vector<rm_terminal::MatchAlert>& out)
        {
            for (const auto& alert : out)
                if (alert.kind == AlertKind::Fortress)
                    return alert.banner;
            return QString();
        };
        auto alerts = red_alerts();
        auto snapshot = in_match(200);
        set<std::uint32_t>(snapshot.game.red_base_status, 1u);
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 0);
        alerts.observe(snapshot, 0);
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 1);
        const QString exposed = banner_of(alerts.observe(snapshot, 250));
        check(exposed.startsWith(QStringLiteral("敌方在激活我方堡垒")) && exposed.contains(QStringLiteral("易伤100%")),
              "armor not deployed: the thief is 100% vulnerable");

        auto deployed = red_alerts();
        set<std::uint32_t>(snapshot.game.red_base_status, 2u);
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 0);
        deployed.observe(snapshot, 0);
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 1);
        const QString plain = banner_of(deployed.observe(snapshot, 250));
        check(plain == QStringLiteral("敌方在激活我方堡垒"), "armor deployed: no vulnerability to mention");

        // 雷达看得到站在我方堡垒增益点上的那台:直接写是谁。
        auto named = red_alerts();
        set<std::uint32_t>(snapshot.game.red_base_status, 1u);
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 0);
        named.observe(snapshot, 0);
        snapshot.map_robots.push_back(radar_robot(103, 6.5, 7.4));
        snapshot.map_robots.push_back(radar_robot(104, 20.0, 3.0));
        set<std::int32_t>(snapshot.game.red_fortress_activation_sec, 1);
        const QString who = banner_of(named.observe(snapshot, 250));
        check(who == QStringLiteral("敌方 3 号步兵在激活我方堡垒 · 易伤100%"), "the robot on our fortress is named");
    }

    QString intrusion_banner(const std::vector<rm_terminal::MatchAlert>& out)
    {
        for (const auto& alert : out)
            if (alert.kind == AlertKind::Intrusion)
                return alert.banner;
        return QString();
    }

    void intrusion_names_the_route()
    {
        // 敌方 3 号在蓝方半场 x=16 往红方走,在 y=7 过中线:中央高地。
        auto alerts = red_alerts();
        auto snapshot = in_match(120);
        snapshot.map_robots = {radar_robot(103, 16.0, 7.0)};
        check(intrusion_banner(alerts.observe(snapshot, 1000)).isEmpty(), "still in its own half");
        snapshot.map_robots = {radar_robot(103, 14.1, 7.0)};
        check(intrusion_banner(alerts.observe(snapshot, 1500)).isEmpty(), "within 0.3 m of the midline: not yet");
        snapshot.map_robots = {radar_robot(103, 13.2, 7.2)};
        check(intrusion_banner(alerts.observe(snapshot, 2000)) == QStringLiteral("敌方 3 号步兵从中央高地坡进入我方半场"),
              "crossing the middle band is the central highland");
        snapshot.map_robots = {radar_robot(103, 13.9, 7.2)};
        alerts.observe(snapshot, 2500);
        snapshot.map_robots = {radar_robot(103, 14.5, 7.2)};
        alerts.observe(snapshot, 3000);
        snapshot.map_robots = {radar_robot(103, 13.0, 7.2)};
        check(intrusion_banner(alerts.observe(snapshot, 4000)).isEmpty(), "back and forth within 15 s: once");

        // 4 号从下面那头过中线:隧道。
        snapshot.map_robots = {radar_robot(104, 15.5, 1.0)};
        alerts.observe(snapshot, 5000);
        snapshot.map_robots = {radar_robot(104, 12.8, 1.2)};
        check(intrusion_banner(alerts.observe(snapshot, 5800)) == QStringLiteral("敌方 4 号步兵从隧道进入我方半场"),
              "crossing at the bottom end is the tunnel");

        // 雷达跟丢了 5 秒,再看到就已经在我方半场:不写来路。
        snapshot.map_robots = {radar_robot(101, 16.0, 7.0)};
        alerts.observe(snapshot, 6000);
        snapshot.map_robots = {radar_robot(101, 10.0, 7.0)};
        check(intrusion_banner(alerts.observe(snapshot, 11000)) == QStringLiteral("敌方 1 号重装进入我方半场"),
              "lost track: no route");

        // 雷达情报里我方公路上方的跨越点有车:以它为准。
        snapshot.map_robots = {radar_robot(107, 16.0, 7.0)};
        alerts.observe(snapshot, 12000);
        snapshot.map_robots = {radar_robot(107, 13.0, 7.0)};
        snapshot.radar_intel.emplace();
        snapshot.radar_intel->field_fresh = true;
        snapshot.radar_intel->crossings = 1u << 3;
        check(intrusion_banner(alerts.observe(snapshot, 12500)) == QStringLiteral("敌方 7 号哨兵从起伏路段进入我方半场"),
              "radar crossing bits win over the position guess");

        // 一直在我方半场里、从没见它在自己那半场:不报。
        auto fresh_start = red_alerts();
        snapshot.radar_intel.reset();
        snapshot.map_robots = {radar_robot(103, 10.0, 7.0)};
        check(intrusion_banner(fresh_start.observe(snapshot, 1000)).isEmpty(), "already inside when we started: quiet");
    }

    void field_zones()
    {
        using rm_terminal::FieldZone;
        using rm_terminal::zone_at;
        check(zone_at({2.46, 7.5}, 1) == FieldZone::BaseBuff, "red base buff");
        check(zone_at({25.54, 7.5}, 2) == FieldZone::BaseBuff && !zone_at({25.54, 7.5}, 1), "blue base is blue's only");
        check(zone_at({6.57, 7.5}, 1) == FieldZone::Fortress && zone_at({21.43, 7.5}, 2) == FieldZone::Fortress,
              "fortress on both sides");
        check(zone_at({10.6, 7.5}, 1) == FieldZone::CentralHighland && zone_at({17.4, 7.5}, 2) == FieldZone::CentralHighland,
              "central highland strips");
        check(zone_at({1.0, 1.0}, 1) == FieldZone::SupplyZone && zone_at({27.0, 14.0}, 2) == FieldZone::SupplyZone,
              "supply zones in the corners");
        check(zone_at({1.4, 12.2}, 1) == FieldZone::HighPoint, "red high point");
        check(zone_at({14.0, 7.5}, 1) == FieldZone::AssemblyZone && zone_at({14.0, 7.5}, 2) == FieldZone::AssemblyZone,
              "assembly zone is shared");
        check(!zone_at({14.0, 3.0}, 1), "the middle of the road is nobody's zone");
        check(rm_terminal::route_at_midline(7.5) == rm_terminal::FieldRoute::Highland &&
                  rm_terminal::route_at_midline(1.0) == rm_terminal::FieldRoute::Tunnel &&
                  rm_terminal::route_at_midline(14.0) == rm_terminal::FieldRoute::Tunnel,
              "midline bands");
    }

    void enemy_low_hp_per_robot()
    {
        auto alerts = red_alerts();
        const auto with_hp = [](std::uint32_t id, std::uint32_t faction, std::uint32_t hp)
        {
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

    void kind_names_and_phrases()
    {
        for (const QString& name : rm_terminal::alert_kind_names())
        {
            bool known = false;
            for (const auto kind : {AlertKind::OutpostTemp, AlertKind::ControlBehind, AlertKind::Fortress,
                                    AlertKind::DartGate, AlertKind::Energy,
                                    AlertKind::EnemyLowHp, AlertKind::BaseAttacked, AlertKind::Intrusion})
                known = known || rm_terminal::alert_kind_name(kind) == name;
            check(known, "every config kind name maps to an alert kind");
        }
        const auto phrases = rm_terminal::match_alert_phrases();
        check(phrases.contains(QStringLiteral("控制度落后了")) &&
                  phrases.contains(QStringLiteral("敌方7号哨兵残血")),
              "every speech line is in the pre-synthesis list");
    }

    void banner_queue_orders_and_expires()
    {
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

} // namespace

int main()
{
    outpost_temp_once_then_full_once();
    control_gap_hysteresis();
    timeline_fires_on_time_and_never_backfills();
    dart_gate_quiet_once_our_darts_are_spent();
    alerts_follow_the_seat();
    fortress_and_base();
    fortress_thief_text();
    intrusion_names_the_route();
    field_zones();
    enemy_low_hp_per_robot();
    kind_names_and_phrases();
    banner_queue_orders_and_expires();
    if (failures != 0)
    {
        std::fprintf(stderr, "match_alerts_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("match_alerts_test: all checks passed\n");
    return 0;
}
