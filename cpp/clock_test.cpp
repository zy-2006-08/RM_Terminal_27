#include "clock.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

// Wall-clock ms since the Unix epoch is ~1.7e12 and climbing. A monotonic
// process-relative clock starts near zero. This threshold is the tripwire: it
// cannot be crossed by elapsed runtime, only by someone reinstating
// QDateTime::currentMSecsSinceEpoch() as the MonotonicMs source.
constexpr MonotonicMs kEpochOrderOfMagnitude = 1'000'000'000;

void clock_is_process_relative_not_wall_clock() {
    check(monotonic_now() < kEpochOrderOfMagnitude,
          "monotonic_now is process-relative, not epoch-based");
}

void clock_never_moves_backward() {
    MonotonicMs previous = monotonic_now();
    for (int i = 0; i < 2000; ++i) {
        const MonotonicMs current = monotonic_now();
        if (current < previous) throw std::runtime_error("clock moved backward");
        previous = current;
    }
    check(true, "repeated reads never move backward");
}

void clock_actually_advances() {
    // A clock stuck at a constant would satisfy monotonicity while silently
    // freezing every hysteresis window, so elapsed progress is asserted too.
    const MonotonicMs before = monotonic_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const MonotonicMs after = monotonic_now();
    check(after - before >= 20, "clock advances across a real sleep");
    check(after - before < 5000, "clock does not jump wildly across a short sleep");
}

// Store::snapshot throws when handed an instant before its latest update, so a
// backward-stepping clock would terminate the process mid-match. This is the
// exact failure the monotonic source exists to prevent.
void clock_is_safe_for_the_store_ordering_guard() {
    const MonotonicMs first = monotonic_now();
    const MonotonicMs second = monotonic_now();
    check(second >= first, "successive reads are safe to feed Store::snapshot");
}

}  // namespace

int main() {
    try {
        clock_is_process_relative_not_wall_clock();
        clock_never_moves_backward();
        clock_actually_advances();
        clock_is_safe_for_the_store_ordering_guard();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "clock: all checks passed\n";
    return 0;
}
