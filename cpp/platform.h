#pragma once

namespace rm_terminal {

enum class Platform { MacOS, Linux };

constexpr Platform platform() noexcept {
#if defined(RM_TERMINAL_PLATFORM_MACOS)
    return Platform::MacOS;
#else
    return Platform::Linux;
#endif
}

constexpr const char* platform_name() noexcept {
    return platform() == Platform::MacOS ? "macOS" : "Linux";
}

}
