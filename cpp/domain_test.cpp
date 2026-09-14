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
        std::cout << "All domain checks passed (no assert, no sleep).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
