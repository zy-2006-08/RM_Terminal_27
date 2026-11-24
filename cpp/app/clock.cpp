#include "clock.h"

#include <chrono>

namespace rm_terminal {

MonotonicMs monotonic_now() {
    // Function-local static: zero-initialised once, thread-safely, at first
    // call, which fixes the process-wide origin without an init-order trap.
    static const std::chrono::steady_clock::time_point origin =
        std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - origin)
        .count();
}

}  // namespace rm_terminal
