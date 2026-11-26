#include "clock.h"
#include <chrono>

namespace rm_terminal
{
    MonotonicMs monotonic_now()
    {
        static const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin).count();
    }
} // namespace rm_terminal
