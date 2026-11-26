#include "battle_analysis.h"
#include "buff_ledger.h"
#include "decoder.h"
#include "radar_intel.h"
#include "rm_terminal.pb.h"
#include "roster_pane.h"

#include <cstdio>
#include <string>

namespace
{

    using rm_terminal::BuffTracker;
    using rm_terminal::MonotonicMs;
    using rm_terminal::RobotId;
    using rm_terminal::Snapshot;

    int failures = 0;

    void check(bool condition, const char* label)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }

    void put16(std::string& bytes, std::size_t at, std::uint32_t value)
    {
        bytes[at] = static_cast<char>(value & 0xFF);
        bytes[at + 1] = static_cast<char>((value >> 8) & 0xFF);
    }

    void put32(std::string& bytes, std::size_t at, std::uint32_t value)
    {
        put16(bytes, at, value & 0xFFFF);
        put16(bytes, at + 2, value >> 16);
    }

    // 一块 0x0310:前 32 字节空着(机械臂回传的位置),32 起是雷达情报,照 雷达情报格式.md 摆。
    // 敌方 3 号:防御 25、易伤 0、攻击 150、冷却 +20、回血 0;敌方 4 号无敌;哨兵防御姿态。
    std::string block(std::uint32_t relay_10ms = 2, std::uint32_t buffs_age = 100, std::uint32_t ammo_age = 80)
    {
        std::string bytes(rm_terminal::kRadarIntelOffset + rm_terminal::kRadarIntelBytes, '\0');
        const std::size_t p = rm_terminal::kRadarIntelOffset;
        bytes[p] = 'R';
        bytes[p + 1] = 'I';
        bytes[p + 2] = 1;
        bytes[p + 3] = static_cast<char>(relay_10ms);
        put16(bytes, p + 4, 77);
        put16(bytes, p + 6, ammo_age);
        put16(bytes, p + 8, 120);
        put16(bytes, p + 10, buffs_age);
        // 0x0A03:1 号 12,3 号 230,4 号 180,7 号 400。
        put16(bytes, p + 12, 12);
        put16(bytes, p + 14, 230);
        put16(bytes, p + 16, 180);
        put16(bytes, p + 20, 400);
        // 0x0A04:剩 850,累计 1900。
        put16(bytes, p + 22, 850);
        put16(bytes, p + 24, 1900);
        put32(bytes, p + 26, 0x00ABCDEF);
        put32(bytes, p + 30, 0x00002000);
        // 0x0A05:3 号在偏移 14。
        const std::size_t b = p + 34;
        bytes[b + 14] = 0;
        put16(bytes, b + 15, 20);
        bytes[b + 17] = 25;
        bytes[b + 18] = 0;
        put16(bytes, b + 19, 150);
        bytes[b + 31] = 50; // 哨兵防御 50
        bytes[b + 35] = 2;  // 哨兵防御姿态
        bytes[b + 39] = 2;  // 4 号无敌不虚弱
        put16(bytes, b + 45, 30);
        return bytes;
    }

    template <class T>
    void set(rm_terminal::Field<T>& field, T value)
    {
        field.value = value;
        field.quality = rm_terminal::Quality::Valid;
        field.freshness = rm_terminal::Freshness::Fresh;
    }

    void set_hp(Snapshot& s, std::uint32_t id, std::uint32_t hp)
    {
        rm_terminal::RobotHealth robot;
        robot.id = RobotId{id};
        robot.faction = rm_terminal::faction_of(id);
        set(robot.current_hp, hp);
        set(robot.max_hp, 200u);
        s.robot_health.push_back(robot);
    }

    // 红方 3 号步兵,比赛第 100 秒,八台车都活着。
    Snapshot match(std::int32_t elapsed = 100)
    {
        Snapshot s;
        s.self = RobotId{3};
        set<std::uint32_t>(s.game.current_round, 1);
        set<std::uint32_t>(s.game.current_stage, 4);
        set<std::int32_t>(s.game.stage_elapsed_sec, elapsed);
        set<std::uint32_t>(s.game.rune_status, 1);
        for (const std::uint32_t id : {1u, 3u, 4u, 7u, 101u, 103u, 104u, 107u})
            set_hp(s, id, 200);
        return s;
    }

    bool has_chip(const rm_terminal::BuffBoard& board, std::uint32_t id, const QString& text)
    {
        const auto found = board.chips.find(id);
        if (found == board.chips.end())
            return false;
        for (const rm_terminal::BuffChip& chip : found->second)
            if (chip.text == text)
                return true;
        return false;
    }

    void parses_layout()
    {
        const auto intel = rm_terminal::parse_radar_intel(block());
        check(intel.has_value(), "parse: block with magic RI v1 parses");
        if (!intel)
            return;
        check(intel->body.seq == 77, "parse: seq");
        check(intel->buffs_age_ms == 120u, "parse: buffs age = radar 100 ms + car 2 x 10 ms");
        check(intel->ammo_age_ms == 100u, "parse: ammo age = 80 + 20");
        check(intel->body.robots[0].allowed_ammo == 12 && intel->body.robots[1].allowed_ammo == 230 &&
                  intel->body.robots[2].allowed_ammo == 180 && intel->body.robots[3].allowed_ammo == 400,
              "parse: 0x0A03 skips the reserved slot before the sentry");
        check(intel->body.robots[1].defense_percent == 25 && intel->body.robots[1].attack_percent == 150 &&
                  intel->body.robots[1].cooling == 20,
              "parse: robot 3 buffs at 0x0A05 offset 14");
        check(intel->body.robots[2].state == 2, "parse: robot 4 main state at offset 39");
        check(intel->body.robots[3].defense_percent == 50 && intel->body.sentry_posture == 2, "parse: sentry");
        check(intel->body.gold == 850 && intel->body.total_gold == 1900, "parse: gold");
        check(intel->body.buff_points == 0x00ABCDEF && intel->body.crossings == 0x00002000, "parse: 0x0A04 words");
        check(intel->body.armor_radius == 30, "parse: armor radius");

        std::string never = block();
        put16(never, rm_terminal::kRadarIntelOffset + 6, 0xFFFF);
        const auto partial = rm_terminal::parse_radar_intel(never);
        check(partial && !partial->ammo_age_ms && partial->buffs_age_ms, "parse: 0xFFFF age = section never received");

        check(!rm_terminal::parse_radar_intel(block(255)), "parse: relay 255 = car lost the radar, ignore");
        check(!rm_terminal::parse_radar_intel(std::string(25, '\1')), "parse: arm feedback only, no intel");
        std::string wrong = block();
        wrong[rm_terminal::kRadarIntelOffset + 2] = 2;
        check(!rm_terminal::parse_radar_intel(wrong), "parse: unknown version");
    }

    void decodes_into_store()
    {
        rm_terminal::Store store(2000);
        rm_terminal::Decoder decoder(store, RobotId{3});
        rm::CustomByteBlock message;
        message.set_data(block());
        check(decoder.accept("CustomByteBlock", message.SerializeAsString(), 10000), "decoder: CustomByteBlock accepted");
        Snapshot s = store.snapshot(10000);
        check(s.radar_intel.has_value() && s.radar_intel->buffs_fresh && s.radar_intel->ammo_fresh, "store: fresh");
        check(s.radar_intel && s.radar_intel->age_ms == 100, "store: newest section (ammo) is 100 ms old");
        check(rm_terminal::enemy_allowed_ammo(s, 103) == 230u, "store: enemy 3 allowed ammo");
        check(!rm_terminal::enemy_allowed_ammo(s, 3), "store: own robots never read from intel");
        check(rm_terminal::enemy_invincible(s, 104) && !rm_terminal::enemy_invincible(s, 103), "store: invincible");

        rm::CustomByteBlock arm_only;
        arm_only.set_data(std::string(25, '\1'));
        check(decoder.accept("CustomByteBlock", arm_only.SerializeAsString(), 10500), "decoder: arm-only block is fine");

        // 车丢了雷达:后面的块标 255,不能把时刻往后挪。
        message.set_data(block(255));
        decoder.accept("CustomByteBlock", message.SerializeAsString(), 12000);
        s = store.snapshot(13200);
        check(s.radar_intel && !s.radar_intel->buffs_fresh, "store: 3 s after the radar's last packet it is stale");
        bool stale = false;
        check(rm_terminal::radar_intel_note(s, &stale) == QStringLiteral("雷达情报过期 3秒") && stale, "note: stale text");
        check(!rm_terminal::enemy_allowed_ammo(s, 103), "store: stale ammo not shown");
    }

    void chips_use_intel()
    {
        rm_terminal::Store store(2000);
        rm_terminal::Decoder decoder(store, RobotId{3});
        rm::CustomByteBlock message;
        message.set_data(block());
        decoder.accept("CustomByteBlock", message.SerializeAsString(), 5000);
        Snapshot s = match();
        s.radar_intel = store.snapshot(5000).radar_intel;

        BuffTracker tracker;
        const rm_terminal::BuffBoard board = tracker.update(s, 5000);
        check(has_chip(board, 103, QStringLiteral("防御25%")), "chips: enemy 3 defense without 约");
        check(has_chip(board, 103, QStringLiteral("攻击×1.5")) && has_chip(board, 103, QStringLiteral("冷却+20")),
              "chips: enemy 3 attack and cooling");
        check(has_chip(board, 104, QStringLiteral("无敌")), "chips: enemy 4 invincible from main state");
        check(has_chip(board, 107, QStringLiteral("防御50%")) && has_chip(board, 107, QStringLiteral("防御姿态")),
              "chips: enemy sentry defense and posture");
        // 敌方重装第 100 秒按规则推「约防御50%」;情报说它什么都没有,就不该再推。
        check(board.chips.find(101) == board.chips.end(), "chips: intel says enemy hero has nothing");
        bool stale = true;
        check(rm_terminal::radar_intel_note(s, &stale) == QStringLiteral("雷达情报") && !stale, "note: fresh text");

        Snapshot expired = match();
        expired.radar_intel = store.snapshot(8100).radar_intel;
        BuffTracker later;
        const rm_terminal::BuffBoard fallback = later.update(expired, 8100);
        check(has_chip(fallback, 101, QStringLiteral("约防御50% 80秒")), "chips: stale intel falls back to rule estimates");
        check(!has_chip(fallback, 103, QStringLiteral("防御25%")), "chips: stale values are not shown as current");
    }

    void roster_and_economy()
    {
        rm_terminal::Store store(2000);
        rm_terminal::Decoder decoder(store, RobotId{3});
        rm::CustomByteBlock message;
        message.set_data(block());
        decoder.accept("CustomByteBlock", message.SerializeAsString(), 5000);
        Snapshot s = match();
        set<std::uint32_t>(s.game.economy, 1000);
        s.radar_intel = store.snapshot(5000).radar_intel;

        const std::vector<rm_terminal::RosterEntry> enemies = rm_terminal::build_roster(s, rm_terminal::kBlue);
        bool sentry_ammo = false;
        for (const rm_terminal::RosterEntry& entry : enemies)
            if (entry.id.value == 107)
                sentry_ammo = entry.bullets == 400;
        check(sentry_ammo, "roster: enemy sentry shows allowed ammo 400");

        const auto metrics = rm_terminal::build_analysis_metrics(s);
        check(!metrics.empty() && metrics[0].self_value == 1000 && metrics[0].enemy_value == 850 && metrics[0].has_enemy,
              "analysis: economy gets the enemy side from intel");
    }

} // namespace

int main()
{
    parses_layout();
    decodes_into_store();
    chips_use_intel();
    roster_and_economy();
    if (failures == 0)
        std::puts("radar_intel_test: all passed");
    return failures == 0 ? 0 : 1;
}
