#pragma once

#include "conversion.h"

#include <mutex>

namespace rm_terminal {

class Store {
public:
    explicit Store(MonotonicMs stale_after_ms, std::size_t event_capacity = 50);
    // 开机选定的席位。「我方/敌方」、飞镖遮挡、自机上限血量都按它算;回放跳转 reset()
    // 时保留。由 Decoder 构造时设置,实况和回放走同一条路。
    void setSelf(RobotId self);
    bool apply(const inbound::GameStatus& patch, MonotonicMs now);
    bool apply(const inbound::RobotHealthSet& patch, MonotonicMs now);
    bool apply(const inbound::Event& patch, MonotonicMs now);
    bool apply(const inbound::RobotDynamicStatus& patch, std::optional<RobotId> source, MonotonicMs now);
    bool apply(const inbound::RobotStaticStatus& patch, std::optional<RobotId> source, MonotonicMs now);
    bool apply(const inbound::RobotModuleStatus& patch, std::optional<RobotId> source, MonotonicMs now);
    bool apply(const inbound::RobotPosition& patch, std::optional<RobotId> source, MonotonicMs now);
    bool apply(const inbound::RobotRespawnStatus& patch, std::optional<RobotId> source, MonotonicMs now);
    bool apply(const inbound::RadarSet& patch, MonotonicMs now);
    bool apply(const inbound::TechCoreStatus& patch, MonotonicMs now);
    bool apply(const inbound::DartStatus& patch, MonotonicMs now);
    bool apply(const inbound::SentryStatus& patch, MonotonicMs now);
    bool apply(const inbound::SentryCtrlResult& patch, MonotonicMs now);
    bool apply(const inbound::PathPlan& patch, MonotonicMs now);
    bool apply(const inbound::MapClick& patch, MonotonicMs now);
    Snapshot snapshot(MonotonicMs now) const;
    // 回放跳转从零重建。只重新喂入目标之前的记录会留下目标之后的字段值,
    // 画面会显示「未来」的数据。
    void reset();

    // 保留的地图标记条数。
    static constexpr std::size_t kMapMarkCapacity = 16;

private:
    bool advance(MonotonicMs now);
    void resetRound();
    std::uint32_t selfFaction() const;

    // apply() 跑在 MQTT 网络线程,snapshot() 跑在界面线程。
    mutable std::mutex mutex_;
    Snapshot state_;
    MonotonicMs threshold_;
    std::size_t event_capacity_;
    MonotonicMs latest_ = 0;
    std::optional<RobotId> self_;

    // 本局在本机累计的状态,换局时清零(见 apply(GameStatus))。
    std::optional<std::uint32_t> round_;
    std::optional<std::uint32_t> stage_;
    int enemy_fixed_dart_hits_ = 0;
    std::optional<MonotonicMs> blind_until_;
    std::uint32_t blind_target_ = 0;
    std::map<std::uint32_t, std::uint32_t> max_hp_seen_;
};

}
