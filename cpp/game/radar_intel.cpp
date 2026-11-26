#include "radar_intel.h"

#include <algorithm>
#include <array>

namespace rm_terminal
{
    namespace
    {

        constexpr std::uint32_t kMagicR = 0x52;
        constexpr std::uint32_t kMagicI = 0x49;
        constexpr std::uint32_t kVersion = 1;
        constexpr std::uint32_t kRelayTooOld = 255;
        constexpr std::uint32_t kNeverReceived = 0xFFFF;

        // 包内偏移,对照 雷达情报格式.md 的表。
        constexpr std::size_t kRelayAt = 3;
        constexpr std::size_t kSeqAt = 4;
        constexpr std::size_t kAgesAt = 6;
        constexpr std::size_t kAmmoAt = 12;  // 0x0A03
        constexpr std::size_t kFieldAt = 22; // 0x0A04
        constexpr std::size_t kBuffsAt = 34; // 0x0A05
        // 每台车在信息波里的位置,和 kFieldRobots(1、3、4、7)同序;跳过的是协议的保留位。
        constexpr std::array<std::size_t, 4> kAmmoSlots{0, 2, 4, 8};
        constexpr std::array<std::size_t, 4> kBuffSlots{0, 14, 21, 28};
        constexpr std::array<std::size_t, 4> kStateSlots{36, 38, 39, 40};

        std::uint32_t u8(const std::string& bytes, std::size_t at) { return static_cast<std::uint8_t>(bytes[at]); }
        std::uint32_t u16(const std::string& bytes, std::size_t at) { return u8(bytes, at) | (u8(bytes, at + 1) << 8); }
        std::uint32_t u32(const std::string& bytes, std::size_t at) { return u16(bytes, at) | (u16(bytes, at + 2) << 16); }

        std::optional<std::size_t> enemy_slot(const Snapshot& snapshot, std::uint32_t robot_id)
        {
            if (!snapshot.self || !snapshot.radar_intel)
                return std::nullopt;
            if (faction_of(robot_id) == faction_of(snapshot.self->value))
                return std::nullopt;
            const std::uint32_t number = robot_id > 100 ? robot_id - 100 : robot_id;
            const auto found = std::find(kFieldRobots.begin(), kFieldRobots.end(), number);
            if (found == kFieldRobots.end())
                return std::nullopt;
            return static_cast<std::size_t>(found - kFieldRobots.begin());
        }

    } // namespace

    std::optional<inbound::RadarIntel> parse_radar_intel(const std::string& custom_bytes)
    {
        if (custom_bytes.size() < kRadarIntelOffset + kRadarIntelBytes)
            return std::nullopt;
        const std::string packet = custom_bytes.substr(kRadarIntelOffset, kRadarIntelBytes);
        if (u8(packet, 0) != kMagicR || u8(packet, 1) != kMagicI || u8(packet, 2) != kVersion)
            return std::nullopt;
        const std::uint32_t relay = u8(packet, kRelayAt);
        if (relay == kRelayTooOld)
            return std::nullopt;

        inbound::RadarIntel out;
        const auto age = [&](std::size_t index) -> std::optional<std::uint32_t>
        {
            const std::uint32_t radar_ms = u16(packet, kAgesAt + 2 * index);
            if (radar_ms == kNeverReceived)
                return std::nullopt;
            return radar_ms + relay * 10;
        };
        out.ammo_age_ms = age(0);
        out.field_age_ms = age(1);
        out.buffs_age_ms = age(2);

        RadarIntel& body = out.body;
        body.seq = u16(packet, kSeqAt);
        for (std::size_t i = 0; i < kFieldRobots.size(); ++i)
        {
            EnemyIntel& robot = body.robots[i];
            robot.allowed_ammo = u16(packet, kAmmoAt + kAmmoSlots[i]);
            const std::size_t at = kBuffsAt + kBuffSlots[i];
            robot.heal_percent = u8(packet, at);
            robot.cooling = u16(packet, at + 1);
            robot.defense_percent = u8(packet, at + 3);
            robot.vulnerable_percent = u8(packet, at + 4);
            robot.attack_percent = u16(packet, at + 5);
            robot.state = u8(packet, kBuffsAt + kStateSlots[i]);
        }
        body.sentry_posture = u8(packet, kBuffsAt + 35);
        body.armor_x = u16(packet, kBuffsAt + 41);
        body.armor_y = u16(packet, kBuffsAt + 43);
        body.armor_radius = u16(packet, kBuffsAt + 45);
        body.gold = u16(packet, kFieldAt);
        body.total_gold = u16(packet, kFieldAt + 2);
        body.buff_points = u32(packet, kFieldAt + 4);
        body.crossings = u32(packet, kFieldAt + 8);
        return out;
    }

    const EnemyIntel* enemy_buff_intel(const Snapshot& snapshot, std::uint32_t robot_id)
    {
        const auto slot = enemy_slot(snapshot, robot_id);
        if (!slot || !snapshot.radar_intel->buffs_fresh)
            return nullptr;
        return &snapshot.radar_intel->robots[*slot];
    }

    std::optional<std::uint32_t> enemy_allowed_ammo(const Snapshot& snapshot, std::uint32_t robot_id)
    {
        const auto slot = enemy_slot(snapshot, robot_id);
        if (!slot || !snapshot.radar_intel->ammo_fresh)
            return std::nullopt;
        return snapshot.radar_intel->robots[*slot].allowed_ammo;
    }

    bool enemy_invincible(const Snapshot& snapshot, std::uint32_t robot_id)
    {
        const EnemyIntel* intel = enemy_buff_intel(snapshot, robot_id);
        return intel && (intel->state == 2 || intel->state == 3);
    }

} // namespace rm_terminal
