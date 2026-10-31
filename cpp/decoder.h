#pragma once
#include "store.h"
#include <string>
#include <vector>

namespace rm_terminal {
// 2027 协议的 protobuf 消息 -> Store 补丁。「己方/对方」字段在这里按席位换算成红蓝。
class Decoder final {
public:
    // 构造时把席位告诉 Store:实况和回放都经过这里。
    Decoder(Store& store, RobotId self) : store_(store), self_(self) { store_.setSelf(self); }
    bool accept(const std::string& topic, const std::string& payload, MonotonicMs now);
    // accept() 认识的 topic,也就是终端要订阅的(CustomByteBlock 不经过 Store,另行订阅)。
    static const std::vector<std::string>& topics();
private:
    Store& store_;
    RobotId self_;
};
}
