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
        std::cout << "All domain checks passed (no assert, no sleep).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
