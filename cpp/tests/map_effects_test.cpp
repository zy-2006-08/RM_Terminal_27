#include "map_effects.h"

#include <cstdio>

namespace
{

    using rm_terminal::MapEffect;
    using rm_terminal::MapEffectKind;
    using rm_terminal::MapEffectTracker;
    using rm_terminal::MonotonicMs;
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

    // 红方 3 号的终端,比赛中,四个阵地各归各、两边基地满血。
    Snapshot base_snapshot()
    {
        Snapshot s;
        s.self = rm_terminal::RobotId{3};
        set<std::uint32_t>(s.game.current_stage, 4);
        set<std::uint32_t>(s.game.red_base_hp, 10000);
        set<std::uint32_t>(s.game.blue_base_hp, 10000);
        set<std::uint32_t>(s.game.red_outpost_zone, 2);
        set<std::uint32_t>(s.game.blue_outpost_zone, 4);
        set<std::uint32_t>(s.game.red_base_zone, 2);
        set<std::uint32_t>(s.game.blue_base_zone, 4);
        set<std::int32_t>(s.game.red_fortress_activation_sec, 0);
        set<std::int32_t>(s.game.blue_fortress_activation_sec, 0);
        return s;
    }

    const MapEffect* find(const std::vector<MapEffect>& effects, MapEffectKind kind)
    {
        for (const MapEffect& effect : effects)
            if (effect.kind == kind)
                return &effect;
        return nullptr;
    }

    void event(Snapshot& s, std::int32_t id, MonotonicMs at, std::uint32_t hitter = 0, std::uint32_t target = 0)
    {
        rm_terminal::EventRecord record;
        record.event_id = id;
        record.received_at = at;
        record.dart_hitter = hitter;
        record.dart_target = target;
        s.events.push(record);
    }

    void old_news_is_not_animated()
    {
        MapEffectTracker tracker;
        Snapshot s = base_snapshot();
        event(s, 9, 500, 1, 2);
        check(tracker.update(s, {}, 1000).empty(), "events already there when the terminal opened are skipped");
    }

    void dart_and_gate()
    {
        MapEffectTracker tracker;
        Snapshot s = base_snapshot();
        tracker.update(s, {}, 1000);
        event(s, 9, 1100, 1, 2);
        event(s, 10, 1150);
        const auto effects = tracker.update(s, {}, 1200);
        const MapEffect* dart = find(effects, MapEffectKind::DartFlight);
        check(dart && dart->faction == rm_terminal::kRed && dart->from.x < 3 && dart->at.x > 25,
              "our dart flies from the red launcher to the blue base");
        const MapEffect* gate = find(effects, MapEffectKind::DartGate);
        check(gate && gate->faction == rm_terminal::kBlue && gate->label == QStringLiteral("敌方开闸"), "enemy gate opens");
        check(!find(tracker.update(s, {}, 1100 + rm_terminal::kDartFlightMs + rm_terminal::kDartImpactMs), MapEffectKind::DartFlight),
              "the dart is gone once it has landed and burst");
    }

    void base_zone_fortress()
    {
        MapEffectTracker tracker;
        Snapshot s = base_snapshot();
        tracker.update(s, {}, 1000);
        set<std::uint32_t>(s.game.red_base_hp, 9500);
        set<std::uint32_t>(s.game.blue_outpost_zone, 2);
        set<std::int32_t>(s.game.red_fortress_activation_sec, 3);
        const auto effects = tracker.update(s, {}, 1250);
        const MapEffect* hit = find(effects, MapEffectKind::BaseHit);
        check(hit && hit->faction == rm_terminal::kRed && hit->label == QStringLiteral("-500"), "our base loses 500");
        const MapEffect* flip = find(effects, MapEffectKind::ZoneFlip);
        check(flip && flip->faction == rm_terminal::kRed && flip->label == QStringLiteral("红方完全控制") &&
                  !flip->outline.empty() && flip->at.x > 14,
              "blue outpost zone turns red");
        const MapEffect* steal = find(effects, MapEffectKind::FortressSteal);
        check(steal && steal->faction == rm_terminal::kBlue && steal->progress == 0.3 && steal->at.x < 14 &&
                  steal->label == QStringLiteral("偷堡垒 3/10秒"),
              "blue is stealing our fortress: 3 of 10 seconds");
        check(find(tracker.update(s, {}, 1500), MapEffectKind::BaseHit) != nullptr, "the ripple keeps playing for a while");
        set<std::int32_t>(s.game.red_fortress_activation_sec, 0);
        check(!find(tracker.update(s, {}, 1750), MapEffectKind::FortressSteal), "no longer stolen: ring gone");
    }

    void rune_burst_only_when_fresh()
    {
        MapEffectTracker tracker;
        Snapshot s = base_snapshot();
        rm_terminal::BuffBoard running;
        running.blue.rune = rm_terminal::make_rune_buff(false, 0, 0, 0, true);
        check(!find(tracker.update(s, running, 10000), MapEffectKind::RuneBurst), "a rune that was already on is not news");
        rm_terminal::BuffBoard fresh;
        fresh.red.rune = rm_terminal::make_rune_buff(true, 6, 8.5, 10200, false);
        const auto effects = tracker.update(s, fresh, 10500);
        const MapEffect* burst = find(effects, MapEffectKind::RuneBurst);
        check(burst && burst->faction == rm_terminal::kRed && burst->label == QStringLiteral("我方大能量机关"),
              "our big rune just went on");
    }

    void quiet_outside_the_match()
    {
        MapEffectTracker tracker;
        Snapshot s = base_snapshot();
        tracker.update(s, {}, 1000);
        set<std::uint32_t>(s.game.current_stage, 5);
        set<std::int32_t>(s.game.red_fortress_activation_sec, 3);
        check(tracker.update(s, {}, 1250).empty(), "settlement: nothing on the map");
    }

} // namespace

int main()
{
    old_news_is_not_animated();
    dart_and_gate();
    base_zone_fortress();
    rune_burst_only_when_fresh();
    quiet_outside_the_match();
    if (failures == 0)
        std::puts("map_effects_test: all passed");
    return failures == 0 ? 0 : 1;
}
