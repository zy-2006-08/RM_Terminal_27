#pragma once
#include "store.h"
#include <string>

namespace rm_terminal {
class Decoder final {
public:
    Decoder(Store& store, RobotId source) : store_(store), source_(source) {}
    bool accept(const std::string& topic, const std::string& payload, MonotonicMs now);
private:
    Store& store_;
    RobotId source_;
    std::optional<std::uint64_t> last_event_timestamp_;
};
}
