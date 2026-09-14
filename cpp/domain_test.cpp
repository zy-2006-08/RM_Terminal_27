#include "store.h"
#include "presentation.h"
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

// Pins every value against core/constants.py:80-86. The simulator sends mode 2,
// which the old table rendered as "独立" while it authoritatively means
// "底盘跟随云台" - a wrong readout, so these are asserted value by value rather
// than eyeballed on a screenshot.
void chassis_names_match_authoritative_constants() {
    check(chassis_name(0) == QStringLiteral("停机"), "chassis 0 is 停机");
    check(chassis_name(1) == QStringLiteral("手动驾驶"), "chassis 1 is 手动驾驶");
    check(chassis_name(2) == QStringLiteral("底盘跟随云台"), "chassis 2 is 底盘跟随云台");
    check(chassis_name(3) == QStringLiteral("小陀螺"), "chassis 3 is 小陀螺");
    check(chassis_name(4) == QStringLiteral("自瞄模式"), "chassis 4 is 自瞄模式");
    check(chassis_name(5) == QStringLiteral("未知"), "chassis 5 falls back to 未知");
    check(chassis_name(9) == QStringLiteral("未知"), "chassis 9 falls back to 未知");
    check(chassis_name(2) != QStringLiteral("独立"), "the old wrong name for mode 2 is gone");
}

EventRecord make_event(std::uint64_t timestamp, std::uint32_t level, const char* text) {
    return EventRecord{timestamp, level, std::string{text}, 0};
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
    for (std::uint64_t i = 1; i <= 5; ++i) {
        history.push(make_event(i, 0, i == 1 ? "oldest" : "later"));
    }
    check(history.size() == 3, "size stops at capacity");
    check(history.capacity() == 3, "capacity is fixed");
    check(history.droppedCount() == 2, "dropped count accumulates overwrites");

    const auto recent = history.recent(10);
    check(recent.size() == 3, "only capacity worth of records survive");
    check(recent[0].timestamp_ms == 5, "newest survivor is the last pushed");
    check(recent[2].timestamp_ms == 3, "oldest survivor is capacity back");
    for (const auto& record : recent) {
        check(record.text != "oldest", "the evicted record is really gone");
    }
}

// The domain layer must not interpret or clamp level values: an unknown severity
// from a future protocol has to survive to the UI, not get silently rewritten.
void event_history_preserves_unknown_levels() {
    EventHistory history(4);
    history.push(make_event(1, 0, "info"));
    history.push(make_event(2, 3, "severe"));
    history.push(make_event(3, 9, "future"));
    const auto recent = history.recent(3);
    check(recent[0].level == 9, "unknown level is preserved verbatim");
    check(recent[1].level == 3, "severe level preserved");
    check(recent[2].level == 0, "info level preserved");
}

void event_history_empty_returns_nothing() {
    EventHistory history(5);
    check(history.recent(3).empty(), "empty history returns no records");
    check(history.size() == 0, "empty history has zero size");
    check(history.droppedCount() == 0, "empty history dropped nothing");
}

// The simulator republishes at 5Hz. Without dedup a single alert would evict the
// entire history within seconds.
void store_deduplicates_republished_events() {
    Store store(10, 20);
    inbound::Event event;
    event.timestamp_ms = 1000;
    event.level = 3;
    event.text = std::string{"blinded"};
    for (int i = 0; i < 10; ++i) {
        check(store.apply(event, i), "republished event is accepted");
    }
    const auto snapshot = store.snapshot(10);
    check(snapshot.events.size() == 1, "republished identical event is stored once");
    check(snapshot.event.text.value == std::string{"blinded"},
          "the existing single-event field still tracks the latest event");
}

// Regression guard for the review finding: keying dedup on timestamp alone would
// discard the second event here, and at the start of blinding that could be the
// severe one.
void store_keeps_distinct_events_sharing_a_millisecond() {
    Store store(10, 20);
    inbound::Event first;
    first.timestamp_ms = 500;
    first.level = 0;
    first.text = std::string{"match resumed"};
    check(store.apply(first, 0), "first event accepted");

    inbound::Event second;
    second.timestamp_ms = 500;
    second.level = 3;
    second.text = std::string{"base blinded"};
    check(store.apply(second, 0), "second event in the same millisecond accepted");

    const auto snapshot = store.snapshot(0);
    check(snapshot.events.size() == 2, "same-millisecond distinct events are both kept");
    check(snapshot.events.recent(1)[0].level == 3, "the severe event survived");
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
    dynamic.robot_id = 7;
    dynamic.current_hp = 100;
    check(store.apply(dynamic, std::nullopt, 12), "second stale probe accepted");
    reporter.inspect(store.snapshot(13));
    check(events.size() == 1, "a fresh robot group does not report as stale");
    reporter.inspect(store.snapshot(22));
    check(events.size() == 2, "a newly stale group logs a new transition");
    check(details.back().contains(QLatin1String("robot7.dynamic")), "the new stale group is named");
    check(details.back().contains(QLatin1String("robots=1")), "stale record reports the robot count");
}

inbound::RobotPositionEntry map_entry(std::uint32_t id, std::uint32_t faction,
                                      double x, double y, bool is_self = false) {
    inbound::RobotPositionEntry entry;
    entry.robot_id = id;
    entry.faction = faction;
    entry.x = x;
    entry.y = y;
    entry.yaw = 0.0;
    entry.is_self = is_self;
    return entry;
}

// Blind status is a normal field group, so it must inherit the same
// Missing/Invalid/Stale semantics as every other group rather than a bypass.
void store_tracks_blind_status_like_any_other_group() {
    Store store(10);
    check(store.snapshot(0).blind.self_base_blinded.quality == Quality::Missing,
          "blind starts Missing");
    check(store.snapshot(0).blind.self_base_blinded.freshness == Freshness::NeverReceived,
          "blind starts NeverReceived");

    inbound::BlindStatus blind;
    blind.self_base_blinded = true;
    blind.blind_started_ms = 1000;
    blind.cause = 1;
    check(store.apply(blind, 0), "blind status accepted");

    auto snapshot = store.snapshot(0);
    check(snapshot.blind.self_base_blinded.quality == Quality::Valid, "blind reports Valid");
    check(snapshot.blind.self_base_blinded.value == true, "blind flag value preserved");
    check(snapshot.blind.cause.value == 1, "blind cause preserved");
    check(snapshot.blind.blind_remaining_ms.quality == Quality::Missing,
          "an omitted blind field stays Missing rather than becoming zero");
    check(!snapshot.blind.blind_remaining_ms.value, "omitted blind field holds no value");
    check(store.snapshot(10).blind.self_base_blinded.freshness == Freshness::Stale,
          "blind ages into Stale on the shared threshold");

    inbound::BlindStatus bad;
    bad.cause = 7;
    check(!store.apply(bad, 10), "out of range blind cause rejected");
    snapshot = store.snapshot(10);
    check(snapshot.blind.cause.quality == Quality::Invalid, "invalid cause reported as Invalid");
    check(snapshot.blind.cause.value == 1, "invalid cause does not erase the last valid value");
}

// The position set is a full snapshot, not a delta. Accumulating would grow
// without bound across a match and leave dead robots on the map.
void store_replaces_map_robots_wholesale() {
    Store store(10);
    inbound::RobotPositionSet three;
    three.entries = {map_entry(1, 1, 1, 1), map_entry(2, 1, 2, 2), map_entry(3, 2, 3, 3)};
    check(store.apply(three, 0), "three entry set accepted");
    check(store.snapshot(0).map_robots.size() == 3, "three robots stored");

    inbound::RobotPositionSet one;
    one.entries = {map_entry(7, 2, 5, 5)};
    check(store.apply(one, 1), "single entry set accepted");
    auto snapshot = store.snapshot(1);
    check(snapshot.map_robots.size() == 1, "the set replaces wholesale instead of accumulating");
    check(snapshot.map_robots[0].id.value == 7, "the surviving robot comes from the newest set");

    inbound::RobotPositionSet empty;
    check(store.apply(empty, 2), "an empty set is legal");
    check(store.snapshot(2).map_robots.empty(), "an empty set clears the list");
}

// Unordered delivery must not reorder the drawing sequence, or screenshot
// evidence cannot be compared frame to frame.
void store_sorts_map_robots_by_id() {
    Store store(10);
    inbound::RobotPositionSet set;
    set.entries = {map_entry(104, 2, 1, 1), map_entry(3, 1, 2, 2), map_entry(7, 1, 3, 3)};
    check(store.apply(set, 0), "unordered set accepted");
    const auto snapshot = store.snapshot(0);
    check(snapshot.map_robots.size() == 3, "all three robots kept");
    check(snapshot.map_robots[0].id.value == 3 && snapshot.map_robots[1].id.value == 7 &&
          snapshot.map_robots[2].id.value == 104,
          "map robots are sorted by ascending id");
}

void store_discards_map_entries_without_identity() {
    Store store(10);
    inbound::RobotPositionEntry anonymous;
    anonymous.x = 5.0;
    anonymous.y = 5.0;
    inbound::RobotPositionSet set;
    set.entries = {map_entry(4, 1, 1, 1), anonymous};
    check(!store.apply(set, 0), "a set holding an identity-less entry reports invalid");
    const auto snapshot = store.snapshot(0);
    check(snapshot.map_robots.size() == 1, "the identity-less entry is discarded");
    check(snapshot.map_robots[0].id.value == 4, "the identified entry survives");
    check(snapshot.map_invalid_entries == 1, "the discard is counted");
}

// The single-robot path is module one's verified pipeline. If a simulated set
// could overwrite our own coordinates, a real robot would silently render from
// fabricated data.
void store_keeps_authoritative_self_position() {
    Store store(10);
    inbound::RobotPosition authoritative;
    authoritative.x = 3.0;
    authoritative.y = 4.0;
    authoritative.yaw = 90.0;
    check(store.apply(authoritative, RobotId{3}, 0), "authoritative self position accepted");

    inbound::RobotPositionSet set;
    set.entries = {map_entry(3, 1, 99.0, 99.0, true), map_entry(4, 1, 1.0, 1.0)};
    check(store.apply(set, 0), "set carrying a self entry accepted");

    const auto snapshot = store.snapshot(0);
    const auto& self = snapshot.map_robots[0];
    check(self.id.value == 3 && self.is_self, "the self robot is identified from the set");
    check(self.position.x.value == 3.0, "self x comes from the authoritative path, not the set's 99");
    check(self.position.y.value == 4.0, "self y comes from the authoritative path");
    check(self.position.yaw.value == 90.0, "self yaw comes from the authoritative path");
    check(self.position.x.quality == Quality::Valid, "authoritative self position is Valid");
    check(snapshot.map_robots[1].position.x.value == 1.0,
          "a teammate still uses the coordinates carried by the set");
}

void store_leaves_self_position_missing_without_authoritative_source() {
    Store store(10);
    inbound::RobotPositionSet set;
    set.entries = {map_entry(3, 1, 99.0, 99.0, true)};
    check(store.apply(set, 0), "self entry accepted with no authoritative position yet");
    const auto snapshot = store.snapshot(0);
    check(snapshot.map_robots.size() == 1, "the self robot is still listed");
    check(snapshot.map_robots[0].position.x.quality == Quality::Missing,
          "self position stays Missing instead of falling back to fabricated coordinates");
    check(!snapshot.map_robots[0].position.x.value, "the fabricated 99 never leaks through");
    check(snapshot.map_robots[0].position.x.freshness == Freshness::NeverReceived,
          "a never received self position reports NeverReceived");
}

void store_demotes_duplicate_self_claims() {
    Store store(10);
    inbound::RobotPositionSet set;
    set.entries = {map_entry(9, 1, 1.0, 1.0, true), map_entry(2, 1, 2.0, 2.0, true)};
    check(!store.apply(set, 0), "a set with two self claims reports invalid");
    const auto snapshot = store.snapshot(0);
    check(snapshot.map_robots.size() == 2, "both robots survive");
    check(snapshot.map_robots[0].id.value == 2 && snapshot.map_robots[0].is_self,
          "the lowest id keeps the self identity");
    check(snapshot.map_robots[1].id.value == 9 && !snapshot.map_robots[1].is_self,
          "the higher id is demoted to a teammate");
    check(snapshot.map_invalid_entries == 1, "the duplicate self claim is counted");
    check(snapshot.map_robots[1].position.x.value == 1.0,
          "the demoted robot keeps the coordinates from the set");
}

// Faction is never inferred from robot_id: the 2027 id encoding is unknown, so
// inferring would state a side with false confidence.
void store_normalizes_unknown_faction() {
    Store store(10);
    inbound::RobotPositionEntry absent;
    absent.robot_id = 6;
    absent.x = 2.0;
    absent.y = 2.0;
    inbound::RobotPositionSet set;
    set.entries = {map_entry(5, 7, 1.0, 1.0), absent, map_entry(8, 2, 3.0, 3.0)};
    check(!store.apply(set, 0), "an out of range faction reports invalid");
    const auto snapshot = store.snapshot(0);
    check(snapshot.map_robots.size() == 3, "no entry is dropped over faction alone");
    check(snapshot.map_robots[0].faction == 0, "faction 7 normalises to unknown");
    check(snapshot.map_robots[1].faction == 0, "an absent faction is unknown");
    check(snapshot.map_robots[2].faction == 2, "a legal faction is preserved verbatim");
    check(snapshot.map_invalid_entries == 1,
          "only the out of range faction counts as invalid; absence is not corruption");
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
        check(store.snapshot(0).game.red_score.value == 0, "legitimate zero retained");
        inbound::RobotDynamicStatus dynamic;
        dynamic.current_hp = 100;
        check(!store.apply(dynamic, std::nullopt, 0), "missing robot identity rejected");
        dynamic.robot_id = 1;
        check(store.apply(dynamic, std::nullopt, 0), "embedded identity accepted");
        check(!store.apply(dynamic, RobotId{2}, 0), "identity mismatch rejected");
        dynamic.robot_id = 2;
        dynamic.current_hp = 50;
        check(store.apply(dynamic, std::nullopt, 0), "second robot accepted");
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
        check(snapshot.robots.at(RobotId{2}).dynamic.current_hp.value == 50, "two identities isolated");
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
        inbound::RobotTelemetry telemetry;
        telemetry.confidence = 1.1;
        check(!store.apply(telemetry, RobotId{1}, 10), "confidence range rejected");
        telemetry.confidence = 0;
        telemetry.fire_permit = false;
        check(store.apply(telemetry, RobotId{1}, 10), "telemetry values accepted read only");
        inbound::Event event;
        event.level = 0;
        event.text = std::string{};
        check(store.apply(event, 10), "global event zero and empty text");
        game.current_stage = 6;
        game.red_score.reset();
        check(!store.apply(game, 10), "stage range rejected");
        check(store.snapshot(10).game.red_score.freshness == Freshness::Stale, "event cannot refresh match");
        check(!store.apply(event, 9), "backwards update time rejected");
        bool rejected = false;
        try { store.snapshot(9); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "backwards snapshot time rejected");
        stale_reporter_logs_once_per_transition();
        chassis_names_match_authoritative_constants();
        event_history_orders_newest_first();
        event_history_discards_oldest_when_full();
        event_history_preserves_unknown_levels();
        event_history_empty_returns_nothing();
        store_deduplicates_republished_events();
        store_keeps_distinct_events_sharing_a_millisecond();
        store_tracks_blind_status_like_any_other_group();
        store_replaces_map_robots_wholesale();
        store_sorts_map_robots_by_id();
        store_discards_map_entries_without_identity();
        store_keeps_authoritative_self_position();
        store_leaves_self_position_missing_without_authoritative_source();
        store_demotes_duplicate_self_claims();
        store_normalizes_unknown_faction();
        std::cout << "All domain checks passed (no assert, no sleep).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
