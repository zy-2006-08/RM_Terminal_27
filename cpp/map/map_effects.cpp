#include "map_effects.h"

#include <algorithm>

namespace rm_terminal
{
    namespace
    {

        template <class T>
        std::optional<T> fresh(const Field<T>& field)
        {
            if (field.quality != Quality::Valid || field.freshness != Freshness::Fresh || !field.value)
                return std::nullopt;
            return field.value;
        }

        std::size_t side_index(std::uint32_t faction) { return faction == kBlue ? 1 : 0; }

        // 阵地编码 1/2 红方、3/4 蓝方、0 没人。
        std::uint32_t holder(std::uint32_t code) { return code == 1 || code == 2 ? kRed : code == 3 || code == 4 ? kBlue
                                                                                                                 : 0; }

        QString zone_label(std::uint32_t code)
        {
            switch (code)
            {
            case 1: return QStringLiteral("红方临时控制");
            case 2: return QStringLiteral("红方完全控制");
            case 3: return QStringLiteral("蓝方临时控制");
            case 4: return QStringLiteral("蓝方完全控制");
            default: return QStringLiteral("没人控制");
            }
        }

    } // namespace

    void MapEffectTracker::start(MapEffect effect)
    {
        // 同一种、同一处的还在放就重新开始,不叠一堆。
        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [&](const MapEffect& old)
                                     {
                                         return old.kind == effect.kind && old.at.x == effect.at.x && old.at.y == effect.at.y;
                                     }),
                      active_.end());
        active_.push_back(std::move(effect));
    }

    std::vector<MapEffect> MapEffectTracker::update(const Snapshot& snapshot, const BuffBoard& buffs, MonotonicMs now)
    {
        const GameFields<Field>& g = snapshot.game;
        const bool in_match = fresh(g.current_stage) == std::optional<std::uint32_t>(4);
        const std::uint32_t self_faction = snapshot.self ? faction_of(snapshot.self->value) : kRed;
        const std::array<std::optional<std::uint32_t>, 2> bases{fresh(g.red_base_hp), fresh(g.blue_base_hp)};
        const std::array<std::optional<std::uint32_t>, 4> zones{fresh(g.red_outpost_zone), fresh(g.blue_outpost_zone),
                                                                fresh(g.red_base_zone), fresh(g.blue_base_zone)};
        const std::vector<EventRecord> events = snapshot.events.recent(snapshot.events.size());

        if (primed_ && in_match)
        {
            for (auto it = events.rbegin(); it != events.rend(); ++it)
            {
                if (it->received_at <= last_event_at_)
                    continue;
                if (it->dart_hitter != 0)
                {
                    const std::uint32_t victim = opponent_of(it->dart_hitter);
                    MapEffect dart{MapEffectKind::DartFlight, it->dart_hitter};
                    dart.from = landmark(Landmark::DartLauncher, it->dart_hitter);
                    dart.at = landmark(it->dart_target == 1 ? Landmark::Outpost : Landmark::Base, victim);
                    dart.started = it->received_at;
                    dart.duration = kDartFlightMs + kDartImpactMs;
                    dart.label = QStringLiteral("中镖");
                    start(std::move(dart));
                }
                if (it->event_id == 10)
                {
                    const std::uint32_t enemy = opponent_of(self_faction);
                    MapEffect gate{MapEffectKind::DartGate, enemy};
                    gate.at = landmark(Landmark::DartLauncher, enemy);
                    gate.started = it->received_at;
                    gate.duration = kEffectMs;
                    gate.label = QStringLiteral("敌方开闸");
                    start(std::move(gate));
                }
            }
            for (const std::uint32_t side : {kRed, kBlue})
            {
                const std::size_t i = side_index(side);
                if (bases[i] && last_base_[i] && *bases[i] < *last_base_[i])
                {
                    MapEffect hit{MapEffectKind::BaseHit, side};
                    hit.at = landmark(Landmark::Base, side);
                    hit.started = now;
                    hit.duration = kBaseHitMs;
                    hit.label = QStringLiteral("-%1").arg(*last_base_[i] - *bases[i]);
                    start(std::move(hit));
                }
                const auto& rune = buffs.side(side).rune;
                const std::optional<MonotonicMs> began = rune && rune->timed ? std::optional<MonotonicMs>(rune->started)
                                                                             : std::nullopt;
                // 敌方的要等 3 秒才分得出是谁开的(buff_ledger.h),所以从「刚知道」那一刻放。
                if (began && began != rune_started_[i] && now - *began < kRuneFreshMs)
                {
                    MapEffect burst{MapEffectKind::RuneBurst, side};
                    burst.at = landmark(Landmark::Rune, side);
                    burst.started = now;
                    burst.duration = kEffectMs;
                    burst.label = (side == self_faction ? QStringLiteral("我方") : QStringLiteral("敌方")) +
                                  (rune->big ? QStringLiteral("大能量机关") : QStringLiteral("小能量机关"));
                    start(std::move(burst));
                }
            }
            for (std::size_t zone = 0; zone < zones.size(); ++zone)
            {
                if (!zones[zone] || !last_zones_[zone] || *zones[zone] == *last_zones_[zone])
                    continue;
                const std::uint32_t home = zone % 2 == 0 ? kRed : kBlue;
                const bool outpost = zone < 2;
                MapEffect flip{MapEffectKind::ZoneFlip, holder(*zones[zone])};
                flip.at = landmark(outpost ? Landmark::Outpost : Landmark::Base, home);
                flip.outline = zone_outline(outpost ? FieldZone::CentralHighland : FieldZone::BaseBuff, home);
                flip.started = now;
                flip.duration = kEffectMs;
                flip.label = zone_label(*zones[zone]);
                start(std::move(flip));
            }
        }
        if (!events.empty())
            last_event_at_ = std::max(last_event_at_, events.front().received_at);
        // 刚打开时已经在的(没 primed 时)只记下,不当成刚发生。
        for (const std::uint32_t side : {kRed, kBlue})
        {
            const std::size_t i = side_index(side);
            if (bases[i])
                last_base_[i] = bases[i];
            const auto& rune = buffs.side(side).rune;
            if (rune && rune->timed)
                rune_started_[i] = rune->started;
        }
        for (std::size_t zone = 0; zone < zones.size(); ++zone)
            if (zones[zone])
                last_zones_[zone] = zones[zone];
        primed_ = true;
        if (!in_match)
        {
            active_.clear();
            return {};
        }

        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [now](const MapEffect& effect)
                                     { return now - effect.started >= effect.duration; }),
                      active_.end());
        std::vector<MapEffect> out = active_;
        // 堡垒被偷:不是一下子的事,激活计时大于 0 就一直画。
        for (const std::uint32_t side : {kRed, kBlue})
        {
            const auto sec = fresh(side == kRed ? g.red_fortress_activation_sec : g.blue_fortress_activation_sec);
            if (!sec || *sec <= 0)
                continue;
            MapEffect steal{MapEffectKind::FortressSteal, opponent_of(side)};
            steal.at = landmark(Landmark::Fortress, side);
            steal.outline = zone_outline(FieldZone::Fortress, side);
            steal.started = now;
            steal.progress = std::clamp(*sec / 10.0, 0.0, 1.0);
            steal.label = QStringLiteral("偷堡垒 %1/10秒").arg(*sec);
            out.push_back(std::move(steal));
        }
        return out;
    }

} // namespace rm_terminal
