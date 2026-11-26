#include "buff_ledger.h"

#include "field_zones.h"
#include "match_event.h"
#include "radar_intel.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace rm_terminal
{
    namespace
    {

        constexpr std::uint32_t kStageMatch = 4;
        constexpr MonotonicMs kRuneMatchMs = 3000;
        constexpr MonotonicMs kSmallRuneMs = 45000;
        constexpr MonotonicMs kReviveInvincibleMs = 30000;
        constexpr MonotonicMs kReviveTouchFloorMs = 10000;
        constexpr MonotonicMs kPaidReviveMs = 3000;
        constexpr std::int32_t kHeroEarlySec = 180;
        constexpr std::int32_t kBigRuneFromSec = 180;
        constexpr MonotonicMs kPoweredCapMs = 15000;
        constexpr MonotonicMs kSentryStepCapMs = 1000;

        template <class T>
        std::optional<T> fresh(const Field<T>& field)
        {
            if (field.quality != Quality::Valid || field.freshness != Freshness::Fresh || !field.value)
                return std::nullopt;
            return field.value;
        }

        template <class T>
        std::optional<T> valid(const Field<T>& field)
        {
            return field.quality == Quality::Valid ? field.value : std::nullopt;
        }

        int seconds_until(MonotonicMs until, MonotonicMs now)
        {
            return static_cast<int>(std::max<MonotonicMs>(0, until - now + 999) / 1000);
        }

        // 协议只说「百分比」。规则把攻击增益写成 150%、200%(伤害 ×1.5、×2),也可能发的是多出来的 50:
        // 规则里没有低于原始伤害的攻击增益,所以不到 100 的按「多出来的」算。
        int times_percent(std::int32_t level) { return level >= 100 ? level : 100 + std::max(0, level); }

        QString times_text(int percent) { return QString::number(percent / 100.0, 'g', 3); }

        struct Value
        {
            int amount = 0;
            std::optional<int> seconds;
            bool estimated = false;

            void offer(int candidate, std::optional<int> candidate_seconds, bool candidate_estimated)
            {
                if (candidate < amount || candidate <= 0)
                    return;
                if (candidate == amount && (candidate_estimated || !estimated))
                    return;
                amount = candidate;
                seconds = candidate_seconds;
                estimated = candidate_estimated;
            }
        };

        struct Effects
        {
            Value defense, vulnerable, attack, cooling_plus, cooling_times, heal, power, invincible;
            bool maybe_invincible = false;
            bool weak = false;
            bool marked = false;
            bool highland_pending = false;
            QString posture;
        };

        QString chip_text(QString body, const Value& value)
        {
            if (value.estimated)
                body.prepend(QStringLiteral("约"));
            if (value.seconds && *value.seconds > 0)
                body += QStringLiteral(" %1秒").arg(*value.seconds);
            return body;
        }

        std::vector<BuffChip> chips_of(const Effects& e)
        {
            std::vector<BuffChip> chips;
            if (e.invincible.amount > 0)
                chips.push_back({chip_text(QStringLiteral("无敌"), e.invincible), BuffTone::Good});
            else if (e.maybe_invincible)
                chips.push_back({QStringLiteral("可能无敌"), BuffTone::Good});
            if (e.weak)
                chips.push_back({QStringLiteral("虚弱"), BuffTone::Bad});
            const int net = e.defense.amount - e.vulnerable.amount;
            if (net > 0)
            {
                Value shown = e.defense;
                shown.estimated = e.defense.estimated || (e.vulnerable.amount > 0 && e.vulnerable.estimated);
                chips.push_back({chip_text(QStringLiteral("防御%1%").arg(net), shown), BuffTone::Good});
            }
            else if (net < 0)
            {
                Value shown = e.vulnerable;
                shown.estimated = e.vulnerable.estimated || (e.defense.amount > 0 && e.defense.estimated);
                chips.push_back({chip_text(QStringLiteral("易伤%1%").arg(-net), shown), BuffTone::Bad});
            }
            if (!e.posture.isEmpty())
                chips.push_back({e.posture, BuffTone::Neutral});
            if (e.attack.amount > 100)
                chips.push_back({chip_text(QStringLiteral("攻击×%1").arg(times_text(e.attack.amount)), e.attack), BuffTone::Good});
            if (e.cooling_plus.amount > 0)
                chips.push_back({chip_text(QStringLiteral("冷却+%1").arg(e.cooling_plus.amount), e.cooling_plus), BuffTone::Good});
            else if (e.cooling_times.amount > 100)
                chips.push_back(
                    {chip_text(QStringLiteral("冷却×%1").arg(times_text(e.cooling_times.amount)), e.cooling_times), BuffTone::Good});
            if (e.heal.amount > 0)
                chips.push_back({chip_text(QStringLiteral("回血%1%").arg(e.heal.amount), e.heal), BuffTone::Good});
            if (e.power.amount > 100)
                chips.push_back({chip_text(QStringLiteral("功率×%1").arg(times_text(e.power.amount)), e.power), BuffTone::Neutral});
            if (e.marked)
                chips.push_back({QStringLiteral("被标记"), BuffTone::Bad});
            if (e.highland_pending)
                chips.push_back({QStringLiteral("高地差一步"), BuffTone::Neutral});
            return chips;
        }

        void apply_records(Effects& e, const std::vector<BuffRecord>& records, MonotonicMs now)
        {
            for (const BuffRecord& record : records)
            {
                std::optional<int> seconds;
                if (record.left_time)
                {
                    const auto elapsed = static_cast<std::int64_t>((now - record.received_at) / 1000);
                    seconds = static_cast<int>(std::max<std::int64_t>(0, static_cast<std::int64_t>(*record.left_time) - elapsed));
                }
                switch (record.type)
                {
                case 1: e.attack.offer(times_percent(record.level), seconds, false); break;
                case 2:
                    if (record.level > 0)
                        e.defense.offer(record.level, seconds, false);
                    if (record.level < 0)
                        e.vulnerable.offer(-record.level, seconds, false);
                    break;
                case 3: e.cooling_plus.offer(record.level, seconds, false); break;
                case 4: e.power.offer(times_percent(record.level), seconds, false); break;
                case 5: e.heal.offer(record.level, seconds, false); break;
                case 7: e.highland_pending = true; break;
                default: break;
                }
            }
        }

        void apply_rune(Effects& e, const RuneBuff& rune, MonotonicMs now)
        {
            if (!rune.active(now))
                return;
            const std::optional<int> seconds = rune.timed ? std::optional<int>(seconds_until(rune.until, now)) : std::nullopt;
            e.defense.offer(rune.defense_percent, seconds, rune.estimated);
            e.attack.offer(rune.attack_percent, seconds, rune.estimated);
            e.cooling_times.offer(rune.cooling_times * 100, seconds, rune.estimated);
        }

        // 雷达情报里的是服务器算好的实际值(已经含能量机关、地形、姿态),不再按规则推。
        // 协议不给剩余时长,标签不写秒。
        void apply_intel(Effects& e, const EnemyIntel& intel)
        {
            if (intel.state == 2 || intel.state == 3)
                e.invincible.offer(1, std::nullopt, false);
            if (intel.state == 3)
                e.weak = true;
            e.defense.offer(static_cast<int>(intel.defense_percent), std::nullopt, false);
            e.vulnerable.offer(static_cast<int>(intel.vulnerable_percent), std::nullopt, false);
            if (intel.attack_percent > 0)
                e.attack.offer(times_percent(static_cast<std::int32_t>(intel.attack_percent)), std::nullopt, false);
            e.cooling_plus.offer(static_cast<int>(intel.cooling), std::nullopt, false);
            e.heal.offer(static_cast<int>(intel.heal_percent), std::nullopt, false);
        }

        std::optional<FieldPoint> map_position(const Snapshot& snapshot, std::uint32_t id)
        {
            for (const MapRobot& robot : snapshot.map_robots)
            {
                if (robot.id.value != id)
                    continue;
                const auto x = fresh(robot.position.x);
                const auto y = fresh(robot.position.y);
                if (x && y)
                    return FieldPoint{*x, *y};
            }
            return std::nullopt;
        }

        // 按雷达位置推的场地增益点(field_zones.h,规则 5.6.4)。站上去不一定激活了(要检测到场地交互卡),
        // 全都算「约」。
        void apply_zone(Effects& e, FieldPoint at, std::uint32_t faction, std::uint32_t number, const GameFields<Field>& game)
        {
            const bool red = faction == kRed;
            const std::optional<FieldZone> zone = zone_at(at, faction);
            if (zone)
                switch (*zone)
                {
                case FieldZone::BaseBuff:
                case FieldZone::HighPoint: e.defense.offer(50, std::nullopt, true); break;
                case FieldZone::CentralHighland: e.defense.offer(25, std::nullopt, true); break;
                case FieldZone::Fortress:
                {
                    // 自己的前哨阵地不在自己手里时,自己的堡垒增益点才生效。
                    const auto outpost = valid(red ? game.red_outpost_zone : game.blue_outpost_zone);
                    const bool own_outpost = outpost && (red ? (*outpost == 1 || *outpost == 2) : (*outpost == 3 || *outpost == 4));
                    if (outpost && !own_outpost)
                        e.defense.offer(50, std::nullopt, true);
                    break;
                }
                case FieldZone::SupplyZone:
                    e.heal.offer(10, std::nullopt, true);
                    if (number == 1)
                        e.invincible.offer(1, std::nullopt, true);
                    break;
                case FieldZone::AssemblyZone:
                    if (number == 1)
                        e.maybe_invincible = true;
                    break;
                case FieldZone::OutpostBuff: break;
                }
            // 站在对方堡垒增益点上、对方堡垒正被激活、对方基地护甲还没展开:100% 易伤。
            const bool other_red = !red;
            const auto activation = valid(other_red ? game.red_fortress_activation_sec : game.blue_fortress_activation_sec);
            const auto other_base = valid(other_red ? game.red_base_status : game.blue_base_status);
            if (zone_at(at, opponent_of(faction)) == FieldZone::Fortress && activation && *activation > 0 && other_base != 2u)
                e.vulnerable.offer(100, std::nullopt, true);
        }

        void apply_posture(Effects& e, const SentryPostureInfo& sentry)
        {
            if (sentry.posture == 2)
                e.defense.offer(sentry.powered ? 99 : sentry.weakened ? 25
                                                                      : 50,
                                std::nullopt, false);
            if (sentry.posture == 1 || sentry.posture == 3)
                e.vulnerable.offer(25, std::nullopt, false);
        }

    } // namespace

    RuneBuff make_rune_buff(bool big, std::uint32_t arms, double rings, MonotonicMs started, bool estimated)
    {
        RuneBuff rune;
        rune.big = big;
        rune.started = started;
        rune.estimated = estimated;
        if (!big)
        {
            rune.defense_percent = 25;
            rune.until = started + kSmallRuneMs;
            return rune;
        }
        // 表 5-22,没收到环数时按最低一档。
        struct Row
        {
            double above;
            int attack, defense, cooling;
        };
        static constexpr Row rows[] = {{9, 300, 50, 5}, {8, 200, 25, 3}, {7, 200, 25, 2}, {3, 150, 25, 2}};
        rune.attack_percent = 150;
        rune.defense_percent = 25;
        for (const Row& row : rows)
            if (rings > row.above)
            {
                rune.attack_percent = row.attack;
                rune.defense_percent = row.defense;
                rune.cooling_times = row.cooling;
                break;
            }
        // 表 5-23:5 臂 30 秒,每多一臂多 5 秒,10 臂 60 秒。
        int seconds = 30;
        if (arms >= 10)
            seconds = 60;
        else if (arms > 5)
            seconds = 30 + 5 * static_cast<int>(arms - 5);
        rune.until = started + static_cast<MonotonicMs>(seconds) * 1000;
        return rune;
    }

    QString rune_time_text(const RuneBuff& rune, MonotonicMs now, bool compact)
    {
        if (now >= rune.until)
            return QString();
        if (now < rune.paused_until)
            return (compact ? QStringLiteral("停%1s") : QStringLiteral("暂停 %1秒")).arg(seconds_until(rune.paused_until, now));
        if (!rune.timed)
            return QStringLiteral("生效中");
        return (compact ? QStringLiteral("%1%2s") : QStringLiteral("%1%2秒"))
            .arg(rune.estimated ? QStringLiteral("约") : QString())
            .arg(seconds_until(rune.until, now));
    }

    QString rune_buff_text(const RuneBuff& rune, MonotonicMs now)
    {
        const QString time = rune_time_text(rune, now);
        if (time.isEmpty() || now < rune.paused_until)
            return time;
        return (rune.big ? QStringLiteral("大 ") : QStringLiteral("小 ")) + time;
    }

    QString sentry_posture_effect(std::uint32_t posture, bool powered, bool weakened)
    {
        switch (posture)
        {
        case 1:
            return powered    ? QStringLiteral("不积热量 · 功率1/2 · 易伤25%")
                   : weakened ? QStringLiteral("冷却×2 · 功率1/2 · 易伤25%")
                              : QStringLiteral("冷却×3 · 功率1/2 · 易伤25%");
        case 2:
            return powered    ? QStringLiteral("防御99% · 功率1/2 · 冷却1/3")
                   : weakened ? QStringLiteral("防御25% · 功率1/2 · 冷却1/3")
                              : QStringLiteral("防御50% · 功率1/2 · 冷却1/3");
        case 3:
            return powered    ? QStringLiteral("功率200W · 冷却1/3 · 易伤25%")
                   : weakened ? QStringLiteral("功率×1.2 · 冷却1/3 · 易伤25%")
                              : QStringLiteral("功率×1.5 · 冷却1/3 · 易伤25%");
        default: return QString();
        }
    }

    QString sentry_posture_name(std::uint32_t posture)
    {
        switch (posture)
        {
        case 1: return QStringLiteral("进攻姿态");
        case 2: return QStringLiteral("防御姿态");
        case 3: return QStringLiteral("移动姿态");
        case 4: return QStringLiteral("强化进攻");
        case 5: return QStringLiteral("强化防御");
        case 6: return QStringLiteral("强化移动");
        default: return QString();
        }
    }

    QString radar_intel_note(const Snapshot& snapshot, bool* stale)
    {
        if (stale)
            *stale = false;
        if (!snapshot.radar_intel || !snapshot.radar_intel->age_ms)
            return QString();
        const RadarIntel& intel = *snapshot.radar_intel;
        if (intel.buffs_fresh || intel.ammo_fresh || intel.field_fresh)
            return QStringLiteral("雷达情报");
        if (stale)
            *stale = true;
        const MonotonicMs age = *intel.age_ms;
        if (age >= 60000)
            return QStringLiteral("雷达情报过期 %1分钟").arg(age / 60000);
        return QStringLiteral("雷达情报过期 %1秒").arg(age / 1000);
    }

    void BuffTracker::reset()
    {
        pending_.clear();
        our_status_.reset();
        status_fresh_since_.reset();
        our_activated_at_.reset();
        our_activated_elapsed_.reset();
        our_info_ = PendingRune{};
        our_kind_from_event_ = false;
        our_timed_ = true;
        our_arms_seen_ = 0;
        our_rings_seen_ = 0;
        our_rune_.reset();
        their_rune_.reset();
        rune_event_sides_.clear();
        our_fixed_dart_hits_ = 0;
        their_fixed_dart_hits_ = 0;
        last_hp_.clear();
        revivals_.clear();
        self_buffs_seen_ = false;
        held_ms_.fill(0);
        powered_ms_.fill(0);
        sentry_last_.reset();
    }

    void BuffTracker::rebuildOurRune()
    {
        if (!our_activated_at_)
            return;
        const bool big = our_info_.kind != 0 ? our_info_.kind == 2
                                             : our_info_.arms > 0 || our_activated_elapsed_.value_or(0) >= kBigRuneFromSec;
        const std::uint32_t arms = our_info_.arms != 0 ? our_info_.arms : our_arms_seen_;
        const double rings = our_info_.rings > 0 ? our_info_.rings : our_rings_seen_;
        const MonotonicMs paused = our_rune_ ? our_rune_->paused_until : 0;
        our_rune_ = make_rune_buff(big, arms, rings, *our_activated_at_, false);
        our_rune_->paused_until = paused;
        if (!our_timed_)
        {
            our_rune_->timed = false;
            our_rune_->until = std::numeric_limits<MonotonicMs>::max();
        }
    }

    void BuffTracker::onRuneEvent(const EventRecord& event, std::uint32_t ours)
    {
        const bool rings_event = event.event_id == 3;
        // 我方刚激活:补上这次还没收到的那一条(大能量机关先后来事件 3 和 4)。终端打开时就已经是已激活的不算:
        // 那一刻不是激活的时刻,紧接着来的事件只可能是敌方的。
        if (our_timed_ && our_activated_at_ && std::llabs(event.received_at - *our_activated_at_) <= kRuneMatchMs)
        {
            if (rings_event && our_info_.arms == 0)
            {
                our_info_.arms = event.rune_arms;
                our_info_.rings = event.rune_rings;
                if (our_info_.kind == 0)
                    our_info_.kind = 2;
                rune_event_sides_[event.received_at] = ours;
                rebuildOurRune();
                return;
            }
            if (!rings_event && !our_kind_from_event_)
            {
                our_info_.kind = event.rune_kind;
                our_kind_from_event_ = true;
                rune_event_sides_[event.received_at] = ours;
                rebuildOurRune();
                return;
            }
        }
        for (PendingRune& pending : pending_)
        {
            if (std::llabs(pending.at - event.received_at) > kRuneMatchMs)
                continue;
            if (rings_event && pending.kind == 2 && pending.arms == 0)
            {
                pending.arms = event.rune_arms;
                pending.rings = event.rune_rings;
                pending.times.push_back(event.received_at);
                return;
            }
            if (!rings_event && event.rune_kind == 2 && pending.kind == 0)
            {
                pending.kind = 2;
                pending.times.push_back(event.received_at);
                return;
            }
        }
        PendingRune pending;
        pending.kind = rings_event ? 0 : event.rune_kind;
        pending.arms = event.rune_arms;
        pending.rings = event.rune_rings;
        pending.at = event.received_at;
        pending.times.push_back(event.received_at);
        pending_.push_back(pending);
    }

    void BuffTracker::onEvent(const EventRecord& event, std::uint32_t ours)
    {
        if (event.rune_kind != 0)
            onRuneEvent(event, ours);
        if (event.dart_hitter == 0)
            return;
        const bool by_us = event.dart_hitter == ours;
        int& hits = by_us ? their_fixed_dart_hits_ : our_fixed_dart_hits_;
        const MonotonicMs blind = dart_blind_ms(event.dart_target, hits);
        if (event.dart_target >= 1 && event.dart_target <= 3)
            ++hits;
        std::optional<RuneBuff>& rune = by_us ? their_rune_ : our_rune_;
        if (!rune)
            return;
        if (event.dart_target == 3)
            rune->paused_until = std::max(rune->paused_until, event.received_at) + blind;
        if (event.dart_target == 4 || event.dart_target == 5)
            rune->until = std::min(rune->until, event.received_at);
    }

    void BuffTracker::trackOurRune(const Snapshot& snapshot, MonotonicMs now, std::uint32_t ours)
    {
        const auto status = fresh(snapshot.game.rune_status);
        if (!status)
        {
            status_fresh_since_.reset();
            // 断线期间可能已经开了:恢复后看到「已激活」不知道是哪一秒开的。
            if (our_status_ != 3u)
                our_status_.reset();
            return;
        }
        if (!status_fresh_since_)
            status_fresh_since_ = now;
        if (*status == 2)
        {
            if (const auto rings = fresh(snapshot.game.rune_average_rings); rings && *rings > 0)
                our_rings_seen_ = *rings;
            if (const auto arms = fresh(snapshot.game.rune_activated_arms); arms && *arms > 0)
                our_arms_seen_ = *arms;
        }
        if (*status == 3 && our_status_ != 3u)
        {
            our_timed_ = our_status_.has_value();
            our_activated_at_ = now;
            our_activated_elapsed_ = fresh(snapshot.game.stage_elapsed_sec);
            our_info_ = PendingRune{};
            our_kind_from_event_ = false;
            our_rune_.reset();
            // 认领前后 3 秒内离得最近的那一次。
            auto best = pending_.end();
            for (auto it = pending_.begin(); it != pending_.end(); ++it)
                if (our_timed_ && std::llabs(it->at - now) <= kRuneMatchMs &&
                    (best == pending_.end() || std::llabs(it->at - now) < std::llabs(best->at - now)))
                    best = it;
            if (best != pending_.end())
            {
                our_info_ = *best;
                our_kind_from_event_ = best->kind != 0;
                for (const MonotonicMs at : best->times)
                    rune_event_sides_[at] = ours;
                pending_.erase(best);
            }
            rebuildOurRune();
        }
        if (*status != 3)
        {
            our_rune_.reset();
            our_activated_at_.reset();
        }
        if (*status == 1)
        {
            our_arms_seen_ = 0;
            our_rings_seen_ = 0;
        }
        our_status_ = status;
    }

    void BuffTracker::settlePending(MonotonicMs now, std::uint32_t ours)
    {
        for (auto it = pending_.begin(); it != pending_.end();)
        {
            if (now - it->at <= kRuneMatchMs)
            {
                ++it;
                continue;
            }
            if (status_fresh_since_ && *status_fresh_since_ <= it->at)
            {
                their_rune_ = make_rune_buff(it->kind == 2 || (it->kind == 0 && it->arms > 0), it->arms, it->rings, it->at, true);
                for (const MonotonicMs at : it->times)
                    rune_event_sides_[at] = opponent_of(ours);
            }
            it = pending_.erase(it);
        }
    }

    void BuffTracker::trackRevivals(const Snapshot& snapshot, MonotonicMs now)
    {
        for (const RobotHealth& robot : snapshot.robot_health)
        {
            auto hp = fresh(robot.current_hp);
            if (snapshot.self && robot.id.value == snapshot.self->value)
            {
                const auto found = snapshot.robots.find(*snapshot.self);
                if (found != snapshot.robots.end())
                    if (const auto dynamic = fresh(found->second.dynamic.current_hp))
                        hp = dynamic;
            }
            if (!hp)
                continue;
            const auto last = last_hp_.find(robot.id.value);
            if (last != last_hp_.end() && last->second == 0 && *hp > 0)
            {
                const auto max = valid(robot.max_hp);
                // 花金币复活回满血,读条复活只回一成(规则 5.3.2)。
                Revival revival{now, max && *max > 0 && *hp * 10 >= *max * 9, std::nullopt};
                if (snapshot.self && robot.id.value == snapshot.self->value)
                    if (const auto found = snapshot.robots.find(*snapshot.self); found != snapshot.robots.end())
                        revival.fired = valid(found->second.dynamic.total_fired);
                revivals_[robot.id.value] = revival;
            }
            if (*hp == 0)
                revivals_.erase(robot.id.value);
            last_hp_[robot.id.value] = *hp;
        }
    }

    void BuffTracker::trackSentry(const Snapshot& snapshot, MonotonicMs now)
    {
        const auto posture = fresh(snapshot.sentry.posture);
        if (!posture || *posture < 1 || *posture > 3)
        {
            sentry_last_.reset();
            return;
        }
        if (sentry_last_)
        {
            const MonotonicMs step = std::clamp<MonotonicMs>(now - *sentry_last_, 0, kSentryStepCapMs);
            held_ms_[*posture] += step;
            if (fresh(snapshot.sentry.powered).value_or(false))
                powered_ms_[*posture] += step;
        }
        sentry_last_ = now;
    }

    BuffBoard BuffTracker::update(const Snapshot& snapshot, MonotonicMs now)
    {
        BuffBoard board;
        if (const auto round = valid(snapshot.game.current_round))
        {
            if (round_ && *round != *round_)
                reset();
            round_ = round;
        }
        if (const auto stage = fresh(snapshot.game.current_stage))
        {
            const bool in_match = *stage == kStageMatch;
            if (in_match != in_match_)
                reset();
            in_match_ = in_match;
        }
        if (!snapshot.self || !in_match_)
            return board;
        const std::uint32_t self_id = snapshot.self->value;
        const std::uint32_t ours = faction_of(self_id);

        const std::vector<EventRecord> events = snapshot.events.recent(snapshot.events.size());
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->received_at > last_event_at_)
                onEvent(*it, ours);
        if (!events.empty())
            last_event_at_ = std::max(last_event_at_, events.front().received_at);

        trackOurRune(snapshot, now, ours);
        settlePending(now, ours);
        trackRevivals(snapshot, now);
        trackSentry(snapshot, now);
        if (snapshot.buffs.count(self_id) != 0)
            self_buffs_seen_ = true;

        board.rune_event_sides = rune_event_sides_;
        // 终端打开时我方能量机关已经生效、又没看到事件 3 的环数:队友按最低档推会和我自己收到的数不一致。
        // 攻击增益只有能量机关会给,直接取我自己那条;防御还混着地形,不动。
        if (our_rune_ && our_info_.rings <= 0)
            if (const auto records = snapshot.buffs.find(self_id); records != snapshot.buffs.end())
                for (const BuffRecord& record : records->second)
                    if (record.type == 1)
                        our_rune_->attack_percent = std::max(our_rune_->attack_percent, record.level);
        SideBuffs& mine = ours == kRed ? board.red : board.blue;
        SideBuffs& other = ours == kRed ? board.blue : board.red;
        if (our_rune_ && now < our_rune_->until)
            mine.rune = our_rune_;
        if (their_rune_ && now < their_rune_->until)
            other.rune = their_rune_;

        const auto offer_structure = [](SideBuffs& side, int percent, const QString& source, bool estimated)
        {
            if (percent <= side.structure_defense)
                return;
            side.structure_defense = percent;
            side.structure_source = source;
            side.structure_estimated = estimated;
        };
        for (SideBuffs* side : {&mine, &other})
            if (side->rune && side->rune->active(now))
                offer_structure(*side, side->rune->defense_percent,
                                side->rune->big ? QStringLiteral("大能量机关") : QStringLiteral("小能量机关"),
                                side->rune->estimated);
        const std::uint32_t tech = valid(snapshot.game.tech_level).value_or(0);
        if (tech >= 4)
            offer_structure(mine, 50, QStringLiteral("装配四级"), false);
        else if (tech >= 3)
            offer_structure(mine, 25, QStringLiteral("装配三级"), false);

        const auto posture = fresh(snapshot.sentry.posture);
        if (posture && *posture >= 1 && *posture <= 3)
        {
            SentryPostureInfo info;
            info.posture = *posture;
            info.powered = fresh(snapshot.sentry.powered).value_or(false);
            info.weakened = fresh(snapshot.sentry.weakened).value_or(false);
            info.held_sec = static_cast<std::int32_t>(held_ms_[*posture] / 1000);
            info.powered_left_sec =
                static_cast<std::int32_t>(std::max<MonotonicMs>(0, kPoweredCapMs - powered_ms_[*posture]) / 1000);
            info.effect = sentry_posture_effect(info.posture, info.powered, info.weakened);
            board.sentry = info;
        }

        const auto elapsed = fresh(snapshot.game.stage_elapsed_sec);
        const MonotonicMs touched_at = [&]() -> MonotonicMs
        {
            const auto found = snapshot.robots.find(*snapshot.self);
            if (found == snapshot.robots.end())
                return 0;
            return found->second.buffs.can_exchange_ammo.last_valid.value_or(0);
        }();

        for (const std::uint32_t faction : {kRed, kBlue})
        {
            for (const std::uint32_t number : kFieldRobots)
            {
                const std::uint32_t id = faction == kBlue ? number + 100 : number;
                const bool is_self = id == self_id;
                const bool own = faction == ours;
                const auto health = std::find_if(snapshot.robot_health.begin(), snapshot.robot_health.end(),
                                                 [id](const RobotHealth& robot)
                                                 { return robot.id.value == id; });
                std::optional<std::uint32_t> hp = health != snapshot.robot_health.end() ? fresh(health->current_hp)
                                                                                        : std::nullopt;
                if (is_self)
                    if (const auto found = snapshot.robots.find(*snapshot.self); found != snapshot.robots.end())
                        if (const auto dynamic = fresh(found->second.dynamic.current_hp))
                            hp = dynamic;
                if (!hp || *hp == 0)
                    continue;

                Effects e;
                const auto records = snapshot.buffs.find(id);
                const bool from_server = records != snapshot.buffs.end() || (is_self && self_buffs_seen_);
                const EnemyIntel* intel = own ? nullptr : enemy_buff_intel(snapshot, id);
                if (records != snapshot.buffs.end())
                    apply_records(e, records->second, now);
                if (intel)
                {
                    apply_intel(e, *intel);
                    if (number == 7)
                        e.posture = sentry_posture_name(snapshot.radar_intel->sentry_posture);
                }
                else if (!from_server)
                {
                    const SideBuffs& side = own ? mine : other;
                    if (side.rune)
                        apply_rune(e, *side.rune, now);
                    if (own && tech >= 3)
                        e.defense.offer(tech >= 4 ? 50 : 25, std::nullopt, false);
                    if (number == 1 && elapsed && *elapsed < kHeroEarlySec && !(own && snapshot.launcher_installed))
                        e.defense.offer(50, kHeroEarlySec - *elapsed, !own);
                    if (own && number == 7 && board.sentry)
                        apply_posture(e, *board.sentry);
                    if (!is_self)
                        if (const auto at = map_position(snapshot, id))
                            apply_zone(e, *at, faction, number, snapshot.game);
                }
                if (!own)
                    for (const MapRobot& robot : snapshot.map_robots)
                        if (robot.id.value == id && robot.highlight != 0 && robot.position.x.freshness == Freshness::Fresh)
                            e.marked = true;

                if (const auto revival = revivals_.find(id); revival != revivals_.end() && !intel)
                {
                    const Revival& r = revival->second;
                    if (is_self)
                    {
                        const bool touched = touched_at >= r.at;
                        MonotonicMs end = r.at + (r.paid ? kPaidReviveMs : kReviveInvincibleMs);
                        if (!r.paid && touched)
                            end = std::min(end, std::max(touched_at, r.at + kReviveTouchFloorMs));
                        if (now < end)
                            e.invincible.offer(1, seconds_until(end, now), false);
                        const auto self_state = snapshot.robots.find(*snapshot.self);
                        const auto fired = self_state != snapshot.robots.end()
                                               ? valid(self_state->second.dynamic.total_fired)
                                               : std::nullopt;
                        const bool shot_since = r.fired && fired && *fired > *r.fired;
                        e.weak = r.paid ? now < r.at + kPaidReviveMs : !touched && !shot_since;
                    }
                    else if (r.paid)
                    {
                        if (now < r.at + kPaidReviveMs)
                            e.invincible.offer(1, seconds_until(r.at + kPaidReviveMs, now), true);
                    }
                    else if (now < r.at + kReviveTouchFloorMs)
                    {
                        // 别的车踩没踩增益点看不到:前 10 秒一定无敌,10–30 秒只是可能。
                        e.invincible.offer(1, seconds_until(r.at + kReviveTouchFloorMs, now), true);
                    }
                    else if (now < r.at + kReviveInvincibleMs)
                    {
                        e.maybe_invincible = true;
                    }
                }

                if (is_self)
                {
                    board.self_weak = e.weak;
                    board.self_attack = std::max(100, e.attack.amount) / 100.0;
                }
                std::vector<BuffChip> chips = chips_of(e);
                if (!chips.empty())
                    board.chips[id] = std::move(chips);
            }
        }
        return board;
    }

} // namespace rm_terminal
