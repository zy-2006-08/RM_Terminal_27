#pragma once

#include "buff_ledger.h"
#include "domain.h"
#include "field_zones.h"

#include <QString>

#include <array>
#include <optional>
#include <vector>

namespace rm_terminal
{

    // 地图上的事件动画:把这一刻发生的事(飞镖命中、敌方开闸、能量机关激活、基地掉血、堡垒被偷、
    // 阵地易手)变成「在哪儿、什么颜色、从什么时候开始、持续多久」。纯逻辑,怎么画归地图。
    enum class MapEffectKind
    {
        DartFlight,    // 从命中方的飞镖发射井划一条弧到被打的前哨站或基地,落地炸一下
        DartGate,      // 敌方飞镖闸门开了,敌方发射井那儿一圈圈地闪
        RuneBurst,     // 能量机关刚激活,正中那儿转一圈放光
        BaseHit,       // 基地掉血,基地那儿一圈红波纹
        FortressSteal, // 堡垒正被偷,堡垒外面一圈进度(激活了几秒 / 10 秒),一直画到不偷了
        ZoneFlip,      // 阵地换手,那一块涂成新主人的颜色,慢慢褪掉
    };

    struct MapEffect
    {
        MapEffectKind kind;
        std::uint32_t faction = 0; // 用谁的颜色;0 = 中立
        FieldPoint from;           // 只有飞镖用:发射井
        FieldPoint at;
        std::vector<FieldPoint> outline;
        MonotonicMs started = 0;
        MonotonicMs duration = 0; // 0 = 一直画,由下一次 update 决定还在不在
        double progress = 0;      // 堡垒:激活了几成
        QString label;
    };

    // 飞镖飞多久、落地炸多久;其余动画各自的时长。
    constexpr MonotonicMs kDartFlightMs = 1600;
    constexpr MonotonicMs kDartImpactMs = 1200;
    constexpr MonotonicMs kEffectMs = 3000;
    constexpr MonotonicMs kBaseHitMs = 1500;
    // 能量机关开了这么久以内才算刚开,再晚(终端刚连上)就不放了。
    constexpr MonotonicMs kRuneFreshMs = 8000;

    class MapEffectTracker
    {
    public:
        // 每个刷新周期调一次,返回现在该画的动画。只认这一次新出现的事,终端刚打开时已经在的不补。
        std::vector<MapEffect> update(const Snapshot& snapshot, const BuffBoard& buffs, MonotonicMs now);

    private:
        void start(MapEffect effect);

        std::vector<MapEffect> active_;
        bool primed_ = false;
        MonotonicMs last_event_at_ = 0;
        std::array<std::optional<std::uint32_t>, 2> last_base_{};  // 红、蓝基地血量
        std::array<std::optional<std::uint32_t>, 4> last_zones_{}; // 红前哨、蓝前哨、红基地、蓝基地阵地
        std::array<std::optional<MonotonicMs>, 2> rune_started_{}; // 红、蓝能量机关这次是什么时候开的
    };

} // namespace rm_terminal
