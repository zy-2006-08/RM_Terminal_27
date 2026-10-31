#include "store.h"
#include "presentation.h"
#include "roster_pane.h"
#include "logging.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace rm_terminal;

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

// Clears the global sink on every exit path, including a failed check, so the
// captured vectors below can never be written to after they go out of scope.
struct SinkGuard {
    ~SinkGuard() { set_log_sink(nullptr); }
};

void robot_class_names_match_2027_lineup() {
    check(robot_class_name(1) == QStringLiteral("重装"), "1 is 重装");
    check(robot_class_name(2).isEmpty(), "2 is not in the lineup");
    check(robot_class_name(3) == QStringLiteral("步兵"), "3 is 步兵");
    check(robot_class_name(4) == QStringLiteral("步兵"), "4 is 步兵");
    check(robot_class_name(5).isEmpty(), "5 is not in the lineup");
    check(robot_class_name(6) == QStringLiteral("空中"), "6 is 空中");
    check(robot_class_name(7) == QStringLiteral("哨兵"), "7 is 哨兵");
    check(robot_class_name(101) == QStringLiteral("重装"), "101 mirrors 1 as 重装");
    check(robot_class_name(103) == QStringLiteral("步兵"), "103 mirrors 3 as 步兵");
    check(robot_class_name(104) == QStringLiteral("步兵"), "104 mirrors 4 as 步兵");
    check(robot_class_name(106) == QStringLiteral("空中"), "106 mirrors 6 as 空中");
    check(robot_class_name(107) == QStringLiteral("哨兵"), "107 mirrors 7 as 哨兵");
    check(display_robot_number(107) == 7, "blue 107 shows as 7");
    check(display_robot_number(101) == 1, "blue 101 shows as 1");
    check(display_robot_number(3) == 3, "red ids are shown unchanged");
    check(faction_of(7) == kRed && faction_of(101) == kBlue && faction_of(111) == kBlue,
          "faction follows the 2027 id table");
}

// build_roster feeds the ally/enemy columns: four field robots per side, the aerial
// seat never takes a row because it has no health.
void roster_splits_the_2027_lineup_by_faction() {
    Snapshot snapshot;
    snapshot.self = RobotId{3};
    for (std::uint32_t id : {1u, 3u, 4u, 7u, 101u, 103u, 104u, 107u}) {
        RobotHealth robot;
        robot.id = RobotId{id};
        robot.faction = faction_of(id);
        robot.current_hp.value = 100;
        robot.max_hp.value = 200;
        snapshot.robot_health.push_back(robot);
    }
    MapRobot aerial;
    aerial.id = RobotId{6};
    aerial.faction = kRed;
    snapshot.map_robots.push_back(aerial);

    const auto red = build_roster(snapshot, kRed);
    const auto blue = build_roster(snapshot, kBlue);
    check(red.size() == 4, "the red column holds the four 2027 field robots");
    check(blue.size() == 4, "the blue column holds the four mirrored ids");
    check(red[0].id.value == 1 && red[1].id.value == 3 && red[3].id.value == 7,
          "the red column is sorted by id");
    check(red[1].is_self, "the seat is flagged inside its column");
    check(!blue[1].is_self, "the mirrored id is not us");
    for (const RosterEntry& entry : red)
        check(!robot_class_name(entry.id.value).isEmpty(), "every red id has a 兵种");

    snapshot.self = RobotId{106};
    const auto self = self_entry(snapshot);
    check(self && self->id.value == 106, "the aerial seat still gets its own status entry");
    check(self_faction(snapshot) == kBlue, "self faction comes from the seat");
}

EventRecord make_event(MonotonicMs at, std::uint32_t level, const char* text) {
    EventRecord record;
    record.level = level;
    record.text = text;
    record.received_at = at;
    return record;
}

// recent() must return newest first. Reversed order would put the oldest alert at
// the top of the panel, so the operator reads history as current.
void event_history_orders_newest_first() {
    EventHistory history(10);
    history.push(make_event(1, 0, "first"));
    history.push(make_event(2, 1, "second"));
    history.push(make_event(3, 3, "third"));

    const auto recent = history.recent(3);
    check(recent.size() == 3, "recent returns everything when under capacity");
    check(recent[0].text == "third", "newest record comes first");
    check(recent[2].text == "first", "oldest record comes last");
    check(history.size() == 3, "size counts stored records");
    check(history.droppedCount() == 0, "nothing dropped under capacity");
    check(history.recent(2).size() == 2, "recent honours the max argument");
    check(history.recent(99).size() == 3, "recent clamps to what exists");
}

// Unbounded growth over a whole match is the failure being prevented, so the
// ring must overwrite and report the loss rather than expand.
void event_history_discards_oldest_when_full() {
    EventHistory history(3);
    for (MonotonicMs i = 1; i <= 5; ++i) {
        history.push(make_event(i, 0, i == 1 ? "oldest" : "later"));
    }
    check(history.size() == 3, "size stops at capacity");
    check(history.capacity() == 3, "capacity is fixed");
    check(history.droppedCount() == 2, "dropped count accumulates overwrites");

    const auto recent = history.recent(10);
    check(recent.size() == 3, "only capacity worth of records survive");
    check(recent[0].received_at == 5, "newest survivor is the last pushed");
    check(recent[2].received_at == 3, "oldest survivor is capacity back");
    for (const auto& record : recent) {
        check(record.text != "oldest", "the evicted record is really gone");
    }
}

void event_history_empty_returns_nothing() {
    EventHistory history(5);
    check(history.recent(3).empty(), "empty history returns no records");
    check(history.size() == 0, "empty history has zero size");
    check(history.droppedCount() == 0, "empty history dropped nothing");
}

// 2027 的 Event 是触发式发送,没有 2026 那种 5Hz 重发:两条一样的事件就是两次事件
// (比如同一台车两次击毁同一个对手),都得记下。
void store_keeps_every_triggered_event() {
    Store store(10, 20);
    store.setSelf(RobotId{3});
    inbound::Event kill;
    kill.event_id = 1;
    kill.param = "101,3";
    check(store.apply(kill, 0), "first kill accepted");
    check(store.apply(kill, 0), "the same kill again is a second kill");
    const auto snapshot = store.snapshot(0);
    check(snapshot.events.size() == 2, "both kills are in the history");
    check(snapshot.kills.red == 2, "both kills are counted");
    check(snapshot.events.recent(1)[0].faction == kRed, "kill banner is the killer's side");
    check(snapshot.events.recent(1)[0].text.find("我方 3 号步兵") != std::string::npos,
          "texts are written from the seat's point of view");
}

// Task 5 acceptance: a field aging into Stale must log once per TRANSITION, not
// once per snapshot. The GUI inspects on a 250ms timer, so per-snapshot logging
// would bury every other event under thousands of duplicates during a stall.
void stale_reporter_logs_once_per_transition() {
    std::vector<QString> events;
    std::vector<QString> details;
    SinkGuard guard;
    set_log_sink([&events, &details](LogLevel, const QString& event, const QStringList& fields) {
        events.push_back(event);
        details.push_back(fields.join(QLatin1Char(' ')));
    });

    Store store(10);
    StaleReporter reporter;

    // Given: nothing received, so fields are NeverReceived and not yet stale.
    reporter.inspect(store.snapshot(0));
    check(events.empty(), "never_received is not reported as stale");

    inbound::GameStatus game;
    game.red_score = 3;
    check(store.apply(game, 0), "stale probe seed accepted");

    reporter.inspect(store.snapshot(5));
    check(events.empty(), "a fresh field is not reported as stale");

    // When: the same field is inspected past the staleness window.
    reporter.inspect(store.snapshot(10));
    check(events.size() == 1, "stale transition logs exactly one record");
    check(events.front() == QLatin1String("stale_data"), "stale transition emits the stale_data token");
    check(details.front().contains(QLatin1String("groups=game:1")),
          "stale record names the group and the stale field count");

    // Then: an unchanged stale set stays silent across repeated inspections.
    reporter.inspect(store.snapshot(11));
    reporter.inspect(store.snapshot(12));
    check(events.size() == 1, "unchanged stale set does not repeat");

    // And: a group that goes stale later is a new transition and logs again.
    inbound::RobotDynamicStatus dynamic;
    dynamic.current_hp = 100;
    check(store.apply(dynamic, RobotId{7}, 12), "second stale probe accepted");
    reporter.inspect(store.snapshot(13));
    check(events.size() == 1, "a fresh robot group does not report as stale");
    reporter.inspect(store.snapshot(22));
    check(events.size() == 2, "a newly stale group logs a new transition");
    check(details.back().contains(QLatin1String("robot7.dynamic")), "the new stale group is named");
    check(details.back().contains(QLatin1String("robots=1")), "stale record reports the robot count");
}

inbound::RadarEntry radar_entry(std::uint32_t id, std::optional<double> x, std::optional<double> y) {
    inbound::RadarEntry entry;
    entry.robot_id = id;
    entry.x = x;
    entry.y = y;
    return entry;
}

// The radar message is a full snapshot of the eight robots, not a delta, and the
// drawing order must not depend on delivery order.
void store_replaces_and_sorts_radar_robots() {
    Store store(10);
    store.setSelf(RobotId{3});
    inbound::RadarSet radar;
    radar.entries = {radar_entry(104, 1, 1), radar_entry(3, 2, 2), radar_entry(7, 3, 3)};
    check(store.apply(radar, 0), "radar set accepted");
    auto snapshot = store.snapshot(0);
    check(snapshot.map_robots.size() == 3, "three robots stored");
    check(snapshot.map_robots[0].id.value == 3 && snapshot.map_robots[1].id.value == 7 &&
              snapshot.map_robots[2].id.value == 104,
          "map robots are sorted by ascending id");

    inbound::RadarSet one;
    one.entries = {radar_entry(7, 5, 5)};
    check(store.apply(one, 1), "single entry set accepted");
    snapshot = store.snapshot(1);
    check(snapshot.map_robots.size() == 1, "the set replaces wholesale instead of accumulating");

    inbound::RadarSet anonymous;
    anonymous.entries = {radar_entry(0, 1, 1), radar_entry(4, 1, 1)};
    check(!store.apply(anonymous, 2), "an identity-less entry reports invalid");
    snapshot = store.snapshot(2);
    check(snapshot.map_robots.size() == 1 && snapshot.map_robots[0].id.value == 4,
          "the identity-less entry is discarded");
    check(snapshot.map_invalid_entries == 1, "the discard is counted");
}

// The single-robot path is the only authority for our own coordinates. If the
// radar could overwrite them, a real robot would silently render from somebody
// else's estimate.
void store_keeps_authoritative_self_position() {
    Store store(10);
    store.setSelf(RobotId{3});
    inbound::RadarSet radar;
    radar.entries = {radar_entry(3, 99.0, 99.0), radar_entry(4, 1.0, 1.0)};
    check(store.apply(radar, 0), "radar carrying the seat accepted");
    auto snapshot = store.snapshot(0);
    check(snapshot.map_robots[0].is_self, "the seat is identified");
    check(!snapshot.map_robots[0].position.x.value, "the radar's 99 never becomes our position");
    check(snapshot.map_robots[0].position.x.freshness == Freshness::NeverReceived,
          "a never received self position reports NeverReceived");

    inbound::RobotPosition authoritative;
    authoritative.x = 3.0;
    authoritative.y = 4.0;
    authoritative.yaw = 90.0;
    check(store.apply(authoritative, RobotId{3}, 1), "authoritative self position accepted");
    snapshot = store.snapshot(1);
    const auto& self = snapshot.map_robots[0];
    check(self.id.value == 3 && self.is_self && !self.from_radar, "self is drawn from RobotPosition");
    check(self.position.x.value == 3.0 && self.position.yaw.value == 90.0, "self pose is the authoritative one");
    check(snapshot.map_robots[1].position.x.value == 1.0, "a teammate still uses the radar");
}

void store_carries_base_hp_through_conversion() {
    Store store(10);
    inbound::GameStatus game;
    game.red_base_hp = 10000;
    game.red_base_max_hp = 10000;
    game.blue_base_hp = 9451;
    game.blue_base_max_hp = 10000;
    check(store.apply(game, 0), "base hp update accepted");

    const auto snapshot = store.snapshot(0);
    check(snapshot.game.red_base_hp.value == 10000u, "red base hp reaches the snapshot");
    check(snapshot.game.blue_base_hp.value == 9451u, "blue base hp reaches the snapshot");
    check(snapshot.game.red_base_hp.freshness == Freshness::Fresh,
          "a received base hp is fresh, not NeverReceived");

    inbound::GameStatus zero_max;
    zero_max.red_base_max_hp = 0;
    check(!store.apply(zero_max, 0), "a zero base max hp reports invalid");
    check(store.snapshot(0).game.red_base_max_hp.value == 10000u,
          "the last valid base max hp survives a rejected zero");
}

// 上限血量协议不发:别的车取本局见过的最高血量(不低于兵种下限),换局清零。
void store_estimates_max_hp_per_round() {
    Store store(10);
    store.setSelf(RobotId{3});
    inbound::GameStatus game;
    game.current_round = 1;
    game.current_stage = 4;
    check(store.apply(game, 0), "round one");
    inbound::RobotHealthSet set;
    set.entries = {{103, 250, std::nullopt}, {7, 120, std::nullopt}};
    check(store.apply(set, 1), "health set");
    set.entries = {{103, 90, std::nullopt}, {7, 120, std::nullopt}};
    check(store.apply(set, 2), "damaged");
    auto snapshot = store.snapshot(2);
    check(snapshot.robot_health[1].id.value == 103 && snapshot.robot_health[1].max_hp.value == 250u,
          "max hp remembers the highest hp seen");
    check(snapshot.robot_health[0].max_hp.value == 200u, "never below the sentry floor");
    game.current_round = 2;
    check(store.apply(game, 3), "round two");
    check(store.apply(set, 4), "health in round two");
    check(store.snapshot(4).robot_health[1].max_hp.value == 150u, "the estimate resets with the round");
}

int main() {
    try {
        for (auto threshold : {-1, 0}) {
            bool rejected = false;
            try { Store invalid(threshold); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "non-positive threshold rejected");
        }
        Store store(10);
        check(freshness_text(Freshness::NeverReceived) == "never_received", "missing presentation");
        check(freshness_text(Freshness::Fresh) == "fresh", "fresh presentation");
        check(freshness_text(Freshness::Stale) == "stale", "stale presentation");
        check(!store.snapshot(0).game.red_score.value, "absent is not zero");
        inbound::GameStatus game;
        game.red_score = 0;
        game.current_stage = 4;
        check(store.apply(game, 0), "valid match update");
        check(store.snapshot(0).game.red_score.value == 0u, "legitimate zero retained");
        inbound::RobotDynamicStatus dynamic;
        dynamic.current_hp = 100;
        check(!store.apply(dynamic, std::nullopt, 0), "missing robot identity rejected");
        check(store.apply(dynamic, RobotId{1}, 0), "identified robot accepted");
        dynamic.current_hp = 50;
        check(store.apply(dynamic, RobotId{2}, 0), "second robot accepted");
        inbound::RobotPosition position;
        position.x = 0.0;
        check(!store.apply(position, std::nullopt, 0), "position requires source");
        check(store.apply(position, RobotId{1}, 0), "zero position accepted");
        auto copy = store.snapshot(0);
        position.y = 3.0;
        position.x.reset();
        check(store.apply(position, RobotId{1}, 9), "partial position patch");
        auto snapshot = store.snapshot(10);
        check(snapshot.robots.at(RobotId{1}).position.x.freshness == Freshness::Stale, "exact expiration boundary");
        check(snapshot.robots.at(RobotId{1}).position.y.freshness == Freshness::Fresh, "independent field freshness");
        check(snapshot.robots.at(RobotId{1}).dynamic.current_hp.freshness == Freshness::Stale, "independent group freshness");
        check(!copy.robots.at(RobotId{1}).position.y.value, "copied snapshot isolation");
        check(snapshot.robots.at(RobotId{2}).dynamic.current_hp.value == 50u, "two identities isolated");
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            position.x = invalid;
            position.y.reset();
            check(!store.apply(position, RobotId{1}, 10), "nonfinite input rejected");
        }
        snapshot = store.snapshot(10);
        const auto& x = snapshot.robots.at(RobotId{1}).position.x;
        check(x.value == 0 && x.quality == Quality::Invalid && x.freshness == Freshness::Stale,
              "invalid does not refresh or erase last valid");
        inbound::RobotModuleStatus modules;
        modules.armor = 3;
        check(!store.apply(modules, RobotId{1}, 10), "module range rejected");
        modules.armor = 0;
        check(store.apply(modules, RobotId{1}, 10), "offline is valid zero");
        inbound::Event event;
        event.event_id = 11;
        check(store.apply(event, 10), "a parameterless event is accepted");
        game.current_stage = 6;
        game.red_score.reset();
        check(!store.apply(game, 10), "stage range rejected");
        check(store.snapshot(10).game.red_score.freshness == Freshness::Stale, "event cannot refresh match");
        check(!store.apply(event, 9), "backwards update time rejected");
        bool rejected = false;
        try { store.snapshot(9); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "backwards snapshot time rejected");
        stale_reporter_logs_once_per_transition();
        robot_class_names_match_2027_lineup();
        roster_splits_the_2027_lineup_by_faction();
        event_history_orders_newest_first();
        event_history_discards_oldest_when_full();
        event_history_empty_returns_nothing();
        store_keeps_every_triggered_event();
        store_replaces_and_sorts_radar_robots();
        store_keeps_authoritative_self_position();
        store_carries_base_hp_through_conversion();
        store_estimates_max_hp_per_round();
        std::cout << "All domain checks passed (no assert, no sleep).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
