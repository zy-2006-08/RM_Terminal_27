#pragma once

#include "inbound.h"

#include <cstddef>
#include <optional>
#include <string>

namespace rm_terminal
{

    // CustomByteBlock(0x0310)里雷达情报的位置和长度,字节布局见 雷达情报格式.md。
    constexpr std::size_t kRadarIntelOffset = 32;
    constexpr std::size_t kRadarIntelBytes = 81;

    // 没放情报、版本不认识、或者车标了 255(车太久没收到雷达的新包)时返回空。
    std::optional<inbound::RadarIntel> parse_radar_intel(const std::string& custom_bytes);

    // 只认敌方的车;这一段过期或这一局没收到过时为空。
    const EnemyIntel* enemy_buff_intel(const Snapshot& snapshot, std::uint32_t robot_id);
    std::optional<std::uint32_t> enemy_allowed_ammo(const Snapshot& snapshot, std::uint32_t robot_id);
    bool enemy_invincible(const Snapshot& snapshot, std::uint32_t robot_id);

} // namespace rm_terminal
