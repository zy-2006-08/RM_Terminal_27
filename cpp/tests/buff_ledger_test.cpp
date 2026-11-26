#include "buff_ledger.h"

#include <cstdio>

namespace
{

    using rm_terminal::BuffBoard;
    using rm_terminal::BuffChip;
    using rm_terminal::BuffRecord;
    using rm_terminal::BuffTone;
    using rm_terminal::BuffTracker;
    using rm_terminal::EventRecord;
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

    template <class T>
    void set(rm_terminal::Field<T>& field, T value)
    {
        field.value = value;
        field.quality = rm_terminal::Quality::Valid;
        field.freshness = rm_terminal::Freshness::Fresh;
    }

    void set_hp(Snapshot& s, std::uint32_t id, std::uint32_t hp, std::uint32_t max_hp)
    {
        for (rm_terminal::RobotHealth& robot : s.robot_health)
            if (robot.id.value == id)
            {
                set(robot.current_hp, hp);
                set(robot.max_hp, max_hp);
                return;
            }
        rm_terminal::RobotHealth robot;
        robot.id = RobotId{id};
        robot.faction = rm_terminal::faction_of(id);
        set(robot.current_hp, hp);
        set(robot.max_hp, max_hp);
        s.robot_health.push_back(robot);
    }

    // 红方 3 号步兵的终端,比赛第 200 秒,我方能量机关未激活,八台车都活着。
    Snapshot match(std::uint32_t self_id = 3, std::int32_t elapsed = 200)
    {
        Snapshot s;
        s.self = RobotId{self_id};
        set<std::uint32_t>(s.game.current_round, 1);
        set<std::uint32_t>(s.game.current_stage, 4);
        set<std::int32_t>(s.game.stage_elapsed_sec, elapsed);
        set<std::uint32_t>(s.game.rune_status, 1);
        for (const std::uint32_t id : {1u, 3u, 4u, 7u, 101u, 103u, 104u, 107u})
            set_hp(s, id, 200, 200);
        return s;
    }

    void push_event(Snapshot& s, std::int32_t id, MonotonicMs at, std::uint32_t kind = 0, std::uint32_t arms = 0,
                    double rings = 0, std::uint32_t hitter = 0, std::uint32_t target = 0)
    {
        EventRecord record;
        record.event_id = id;
        record.received_at = at;
        record.rune_kind = kind;
        record.rune_arms = arms;
        record.rune_rings = rings;
        record.dart_hitter = hitter;
        record.dart_target = target;
        s.events.push(record);
    }

    bool has_chip(const BuffBoard& board, std::uint32_t id, const QString& text, BuffTone tone)
    {
        const auto found = board.chips.find(id);
        if (found == board.chips.end())
            return false;
        for (const BuffChip& chip : found->second)
            if (chip.text == text && chip.tone == tone)
                return true;
        return false;
    }

    bool chip_starts(const BuffBoard& board, std::uint32_t id, const QString& prefix)
    {
        const auto found = board.chips.find(id);
        if (found == board.chips.end())
            return false;
        for (const BuffChip& chip : found->second)
            if (chip.text.startsWith(prefix))
                return true;
        return false;
    }

    // 我方小能量机关:状态变成「已激活」,前后 3 秒内的事件 4 算我方;全队和建筑防御 25%,45 秒。
    void our_small_rune()
    {
        BuffTracker tracker;
        Snapshot s = match();
        tracker.update(s, 9000);
        set<std::uint32_t>(s.game.rune_status, 3);
        push_event(s, 4, 9800, 1);
        BuffBoard board = tracker.update(s, 10000);
        check(board.red.rune && !board.red.rune->big && !board.red.rune->estimated, "our small rune is ours, not guessed");
        check(board.red.structure_defense == 25 && board.red.structure_source == QStringLiteral("小能量机关"),
              "base and outpost get 25% from the small rune");
        check(has_chip(board, 4, QStringLiteral("防御25% 45秒"), BuffTone::Good), "a teammate shows the rune defense and time");
        check(!board.blue.rune, "the enemy did not activate anything");
        board = tracker.update(s, 14000);
        check(!board.blue.rune, "the event that matched ours is never handed to the enemy");
        check(rm_terminal::rune_buff_text(*board.red.rune, 14000) == QStringLiteral("小 41秒"), "rune text counts down");
        set<std::uint32_t>(s.game.rune_status, 1);
        board = tracker.update(s, 56000);
        check(!board.red.rune && board.chips.count(4) == 0, "rune over: teammates lose the chip");
    }

    // 敌方小能量机关:事件 4 来了,我方状态一直是未激活,3 秒后算敌方,标「约」。
    void enemy_small_rune()
    {
        BuffTracker tracker;
        Snapshot s = match();
        tracker.update(s, 19000);
        push_event(s, 4, 20000, 1);
        BuffBoard board = tracker.update(s, 21000);
        check(!board.blue.rune, "wait before deciding whose rune it was");
        board = tracker.update(s, 23100);
        check(board.blue.rune && board.blue.rune->estimated && !board.blue.rune->big, "the enemy activated a small rune");
        check(has_chip(board, 103, QStringLiteral("约防御25% 42秒"), BuffTone::Good), "enemy chips say it is estimated");
        check(board.blue.structure_defense == 25 && board.blue.structure_estimated, "enemy base defense is estimated");
        check(board.chips.count(3) == 0, "our robots get nothing from the enemy rune");
        check(board.rune_event_sides.count(20000) == 1 && board.rune_event_sides.at(20000) == rm_terminal::kBlue,
              "the event feed learns the event was the enemy's");
    }

    // 我方能量机关状态断了:分不出是谁的,不归给敌方。
    void unknown_side_is_dropped()
    {
        BuffTracker tracker;
        Snapshot s = match();
        s.game.rune_status.freshness = rm_terminal::Freshness::Stale;
        tracker.update(s, 19000);
        push_event(s, 4, 20000, 1);
        const BuffBoard board = tracker.update(s, 24000);
        check(!board.blue.rune && !board.red.rune, "no rune status: nobody gets the rune");
    }

    // 敌方大能量机关:事件 3 的 8 臂、平均 8.5 环,攻击 ×2、防御 25%、冷却 ×3,45 秒(表 5-22、5-23)。
    void enemy_big_rune()
    {
        BuffTracker tracker;
        Snapshot s = match(3, 260);
        tracker.update(s, 99000);
        push_event(s, 3, 100000, 2, 8, 8.5);
        push_event(s, 4, 100100, 2);
        const BuffBoard board = tracker.update(s, 104000);
        check(board.blue.rune && board.blue.rune->big, "events 3 and 4 make one big rune");
        check(board.blue.rune && board.blue.rune->attack_percent == 200 && board.blue.rune->defense_percent == 25 &&
                  board.blue.rune->cooling_times == 3,
              "8.5 rings: attack x2, defense 25%, cooling x3");
        check(board.blue.rune && board.blue.rune->until == 145000, "8 arms last 45 seconds");
        check(chip_starts(board, 104, QStringLiteral("约攻击×2")), "enemy chip shows the attack buff");
        check(chip_starts(board, 104, QStringLiteral("约冷却×3")), "enemy chip shows the cooling buff");
    }

    void rune_tables()
    {
        const auto rune = [](double rings, std::uint32_t arms)
        { return rm_terminal::make_rune_buff(true, arms, rings, 0, false); };
        check(rune(2.0, 5).attack_percent == 150 && rune(2.0, 5).cooling_times == 1, "[1,3] rings: 150%, no cooling");
        check(rune(5.0, 5).cooling_times == 2, "(3,7] rings: cooling x2");
        check(rune(7.5, 5).attack_percent == 200, "(7,8] rings: attack 200%");
        check(rune(9.6, 10).attack_percent == 300 && rune(9.6, 10).defense_percent == 50 && rune(9.6, 10).cooling_times == 5,
              "(9,10] rings: 300% / 50% / x5");
        check(rune(9.6, 5).until == 30000 && rune(9.6, 7).until == 40000 && rune(9.6, 10).until == 60000,
              "arms 5/7/10 last 30/40/60 seconds");
        check(rm_terminal::make_rune_buff(false, 0, 0, 0, false).until == 45000, "small rune lasts 45 seconds");
    }

    // 我方大能量机关:激活时还没收到事件,按正在激活时看到的环数和臂数算;事件 3 随后到了再按它更正。
    void our_big_rune()
    {
        BuffTracker tracker;
        Snapshot s = match(3, 200);
        tracker.update(s, 1000);
        set<std::uint32_t>(s.game.rune_status, 2);
        set(s.game.rune_average_rings, 9.5);
        set<std::uint32_t>(s.game.rune_activated_arms, 10);
        tracker.update(s, 2000);
        set<std::uint32_t>(s.game.rune_status, 3);
        BuffBoard board = tracker.update(s, 3000);
        check(board.red.rune && board.red.rune->big && board.red.rune->attack_percent == 300, "after 3:00 our rune is the big one");
        check(board.red.structure_defense == 50, "9.5 rings: structures get 50%");
        push_event(s, 3, 3300, 2, 6, 7.5);
        push_event(s, 4, 3400, 2);
        board = tracker.update(s, 3500);
        check(board.red.rune && board.red.rune->attack_percent == 200 && board.red.rune->until == 3000 + 35000,
              "event 3 corrects ours: 7.5 rings and 6 arms");
        board = tracker.update(s, 8000);
        check(!board.blue.rune, "our own events are not mistaken for the enemy");
    }

    // 服务器发的 Buff:同类取最大,防御和易伤相抵;有服务器数据的车不再叠推算。
    void server_records()
    {
        BuffTracker tracker;
        Snapshot s = match();
        const auto add = [&s](std::uint32_t type, std::int32_t level, std::uint32_t max_time, std::uint32_t left)
        {
            BuffRecord record;
            record.type = type;
            record.level = level;
            record.max_time = max_time;
            record.left_time = left;
            record.received_at = 5000;
            s.buffs[3].push_back(record);
        };
        add(2, 25, 45, 40);
        add(2, 50, 30, 22);
        add(1, 200, 45, 40);
        add(3, 40, 120, 100);
        add(7, 0, 0, 0);
        BuffBoard board = tracker.update(s, 5500);
        check(has_chip(board, 3, QStringLiteral("防御50% 22秒"), BuffTone::Good), "same type: the larger one wins");
        check(has_chip(board, 3, QStringLiteral("攻击×2 40秒"), BuffTone::Good), "attack 200% reads x2");
        check(has_chip(board, 3, QStringLiteral("冷却+40 100秒"), BuffTone::Good), "cooling is a direct value");
        check(has_chip(board, 3, QStringLiteral("高地差一步"), BuffTone::Neutral), "terrain pre-buff");
        check(board.self_attack == 2.0, "self attack multiplier");
        add(2, -100, 0, 0);
        board = tracker.update(s, 5600);
        check(has_chip(board, 3, QStringLiteral("易伤50%"), BuffTone::Bad), "50% defense against 100% vulnerability nets to 50% vulnerable");

        s.buffs.clear();
        set<std::uint32_t>(s.game.rune_status, 3);
        board = tracker.update(s, 6000);
        check(board.chips.count(3) == 0, "once the server sent our buffs, an empty list means no buffs");
        check(chip_starts(board, 4, QStringLiteral("防御25%")), "teammates still get the inferred rune");
    }

    // 复活:读条复活无敌最多 30 秒,金币复活 3 秒;自机虚弱到踩到补给区、基地或前哨站增益点。
    void revivals()
    {
        BuffTracker tracker;
        Snapshot s = match();
        set_hp(s, 103, 0, 200);
        set_hp(s, 104, 0, 200);
        set_hp(s, 3, 0, 200);
        tracker.update(s, 1000);
        set_hp(s, 103, 20, 200);
        set_hp(s, 104, 200, 200);
        set_hp(s, 3, 20, 200);
        BuffBoard board = tracker.update(s, 2000);
        check(has_chip(board, 103, QStringLiteral("约无敌 10秒"), BuffTone::Good), "read revive: surely invincible for the first 10 s");
        check(has_chip(board, 104, QStringLiteral("约无敌 3秒"), BuffTone::Good), "paid revive: 3 s");
        check(board.self_weak && has_chip(board, 3, QStringLiteral("虚弱"), BuffTone::Bad), "self is weak after a read revive");
        check(has_chip(board, 3, QStringLiteral("无敌 30秒"), BuffTone::Good), "self invincibility is not a guess");

        set(s.robots[RobotId{3}].buffs.can_exchange_ammo, true);
        s.robots[RobotId{3}].buffs.can_exchange_ammo.last_valid = 8000;
        board = tracker.update(s, 9000);
        check(!board.self_weak, "touching a supply point clears weakness");
        check(has_chip(board, 3, QStringLiteral("无敌 3秒"), BuffTone::Good), "touched before 10 s: invincible until 10 s");
        board = tracker.update(s, 20000);
        check(has_chip(board, 103, QStringLiteral("可能无敌"), BuffTone::Good), "10-30 s after a read revive: maybe invincible");
        board = tracker.update(s, 33000);
        check(board.chips.count(103) == 0, "30 s later the enemy is no longer invincible");
    }

    // 自机复活后发出过弹:发射机构没锁,不是虚弱。
    void weak_ends_when_firing()
    {
        BuffTracker tracker;
        Snapshot s = match();
        set_hp(s, 3, 0, 200);
        set<std::uint32_t>(s.robots[RobotId{3}].dynamic.total_fired, 50);
        tracker.update(s, 1000);
        set_hp(s, 3, 20, 200);
        BuffBoard board = tracker.update(s, 2000);
        check(board.self_weak, "weak right after the revive");
        set<std::uint32_t>(s.robots[RobotId{3}].dynamic.total_fired, 51);
        board = tracker.update(s, 3000);
        check(!board.self_weak, "a shot after the revive means the shooter is unlocked");
    }

    // 重装前 3 分钟、没装上 42mm 时防御 50%:我方知道装没装,敌方是推的。
    void hero_early_defense()
    {
        BuffTracker tracker;
        Snapshot s = match(3, 100);
        BuffBoard board = tracker.update(s, 1000);
        check(has_chip(board, 1, QStringLiteral("防御50% 80秒"), BuffTone::Good), "our hero before 3:00");
        check(has_chip(board, 101, QStringLiteral("约防御50% 80秒"), BuffTone::Good), "enemy hero before 3:00 is estimated");
        s.launcher_installed = true;
        board = tracker.update(s, 2000);
        check(board.chips.count(1) == 0, "42mm installed: no more hero defense");
        set<std::int32_t>(s.game.stage_elapsed_sec, 181);
        board = tracker.update(s, 3000);
        check(board.chips.count(101) == 0, "after 3:00 nobody has it");
    }

    // 我方飞镖命中随机固定目标:敌方能量机关增益暂停 10 秒(第一次命中);命中随机移动目标:没了。
    void dart_suppression()
    {
        BuffTracker tracker;
        Snapshot s = match();
        tracker.update(s, 19000);
        push_event(s, 4, 20000, 1);
        tracker.update(s, 23100);
        push_event(s, 9, 24000, 0, 0, 0, 1, 3);
        BuffBoard board = tracker.update(s, 25000);
        check(board.blue.rune && rm_terminal::rune_buff_text(*board.blue.rune, 25000) == QStringLiteral("暂停 9秒"),
              "the enemy rune is paused");
        check(!chip_starts(board, 103, QStringLiteral("约防御")), "paused buffs do not show on robots");
        board = tracker.update(s, 34500);
        check(chip_starts(board, 103, QStringLiteral("约防御25%")), "after the pause it is back");
        push_event(s, 9, 35000, 0, 0, 0, 1, 4);
        board = tracker.update(s, 35500);
        check(!board.blue.rune, "moving-target hit removes the enemy rune buff");
    }

    // 哨兵姿态(指挥手):累计时间和强化剩余在本机数,效果照表 5-28。
    void sentry_posture()
    {
        BuffTracker tracker;
        Snapshot s = match(6);
        set<std::uint32_t>(s.sentry.posture, 2);
        set(s.sentry.powered, false);
        set(s.sentry.weakened, false);
        for (MonotonicMs t = 0; t <= 10000; t += 250)
            tracker.update(s, t);
        set(s.sentry.powered, true);
        BuffBoard board;
        for (MonotonicMs t = 10250; t <= 16000; t += 250)
            board = tracker.update(s, t);
        check(board.sentry && board.sentry->posture == 2 && board.sentry->held_sec == 16, "posture time adds up");
        check(board.sentry && board.sentry->powered_left_sec == 9, "powered time left out of 15 s");
        check(board.sentry && board.sentry->effect.startsWith(QStringLiteral("防御99%")), "powered defense posture is 99%");
        check(has_chip(board, 7, QStringLiteral("防御99%"), BuffTone::Good), "the sentry row shows the posture defense");
        set<std::uint32_t>(s.sentry.posture, 1);
        set(s.sentry.powered, false);
        board = tracker.update(s, 16250);
        check(has_chip(board, 7, QStringLiteral("易伤25%"), BuffTone::Bad), "attack posture: 25% vulnerable");
    }

    void radar_mark_and_assembly()
    {
        BuffTracker tracker;
        Snapshot s = match();
        rm_terminal::MapRobot marked;
        marked.id = RobotId{104};
        marked.faction = rm_terminal::kBlue;
        marked.highlight = 1;
        set(marked.position.x, 10.0);
        set(marked.position.y, 5.0);
        s.map_robots.push_back(marked);
        set<std::uint32_t>(s.game.tech_level, 3);
        const BuffBoard board = tracker.update(s, 1000);
        check(has_chip(board, 104, QStringLiteral("被标记"), BuffTone::Bad), "a radar-marked enemy");
        check(board.red.structure_defense == 25 && board.red.structure_source == QStringLiteral("装配三级"),
              "level 3 assembly: 25% for structures");
        check(has_chip(board, 4, QStringLiteral("防御25%"), BuffTone::Good), "level 3 assembly: 25% for ground robots");
        check(board.chips.count(103) == 0, "enemy robots get nothing from our assembly");
    }

    // 终端打开时我方能量机关已经是已激活:不知道哪一秒开的,不写还剩几秒。
    void already_active_when_opened()
    {
        BuffTracker tracker;
        Snapshot s = match(3, 100);
        set<std::uint32_t>(s.game.rune_status, 3);
        const BuffBoard board = tracker.update(s, 1000);
        check(board.red.rune && !board.red.rune->timed, "no start time: untimed");
        check(board.red.rune && rm_terminal::rune_buff_text(*board.red.rune, 1000) == QStringLiteral("小 生效中"),
              "untimed text");
        check(has_chip(board, 4, QStringLiteral("防御25%"), BuffTone::Good), "untimed chips carry no seconds");
    }

    // 终端打开时我方已经是已激活,紧接着来的事件 4 是敌方的:不能当成我方这次激活。
    void opened_mid_rune_then_enemy_activates()
    {
        BuffTracker tracker;
        Snapshot s = match(3, 100);
        set<std::uint32_t>(s.game.rune_status, 3);
        tracker.update(s, 1000);
        push_event(s, 4, 2500, 1);
        BuffBoard board = tracker.update(s, 3000);
        check(board.red.rune && !board.red.rune->timed, "ours stays untimed");
        board = tracker.update(s, 6000);
        check(board.blue.rune && board.blue.rune->estimated, "the event right after opening goes to the enemy");
        check(board.rune_event_sides.count(2500) == 1 && board.rune_event_sides.at(2500) == rm_terminal::kBlue,
              "the feed names it the enemy's");
    }

    void outside_match_is_empty()
    {
        BuffTracker tracker;
        Snapshot s = match();
        set_hp(s, 103, 0, 200);
        tracker.update(s, 1000);
        set<std::uint32_t>(s.game.current_stage, 5);
        set_hp(s, 103, 200, 200);
        BuffBoard board = tracker.update(s, 2000);
        check(board.chips.empty() && !board.red.rune && !board.blue.rune, "nothing outside the match");
        set<std::uint32_t>(s.game.current_stage, 4);
        board = tracker.update(s, 3000);
        check(board.chips.count(103) == 0, "a new round does not read as a revival");
    }

    void place(Snapshot& s, std::uint32_t id, double x, double y)
    {
        rm_terminal::MapRobot robot;
        robot.id = RobotId{id};
        robot.faction = rm_terminal::faction_of(id);
        robot.from_radar = true;
        set(robot.position.x, x);
        set(robot.position.y, y);
        s.map_robots.push_back(robot);
    }

    // 按雷达位置推的增益点:红方 3 号终端,比赛第 200 秒(重装开局防御已经没了)。
    void zones_by_position()
    {
        BuffTracker tracker;
        Snapshot s = match();
        place(s, 4, 10.6, 7.5);    // 队友在红方中央高地增益点上
        place(s, 103, 17.4, 7.5);  // 敌方在蓝方中央高地增益点上
        place(s, 7, 2.46, 7.5);    // 我方哨兵在基地增益点上
        place(s, 101, 27.0, 14.0); // 敌方重装在蓝方补给区
        place(s, 104, 6.5, 7.4);   // 敌方 4 号在偷我方堡垒
        set<std::int32_t>(s.game.red_fortress_activation_sec, 3);
        set<std::uint32_t>(s.game.red_base_status, 1u);
        const BuffBoard board = tracker.update(s, 1000);
        check(has_chip(board, 4, QStringLiteral("约防御25%"), BuffTone::Good), "teammate on the central highland strip");
        check(has_chip(board, 103, QStringLiteral("约防御25%"), BuffTone::Good), "enemy on its central highland strip");
        check(has_chip(board, 7, QStringLiteral("约防御50%"), BuffTone::Good), "our sentry on the base buff point");
        check(has_chip(board, 101, QStringLiteral("约无敌"), BuffTone::Good) &&
                  has_chip(board, 101, QStringLiteral("约回血10%"), BuffTone::Good),
              "enemy hero in its supply zone is invincible");
        check(has_chip(board, 104, QStringLiteral("约易伤100%"), BuffTone::Bad), "the robot stealing our fortress");
        check(board.chips.count(3) == 0, "self never uses position guesses");
    }

} // namespace

int main()
{
    our_small_rune();
    enemy_small_rune();
    unknown_side_is_dropped();
    enemy_big_rune();
    rune_tables();
    our_big_rune();
    server_records();
    revivals();
    weak_ends_when_firing();
    hero_early_defense();
    dart_suppression();
    sentry_posture();
    radar_mark_and_assembly();
    already_active_when_opened();
    opened_mid_rune_then_enemy_activates();
    outside_match_is_empty();
    zones_by_position();
    if (failures == 0)
        std::printf("buff_ledger: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
