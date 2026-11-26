#pragma once

#include "domain.h"

#include <QString>

#include <array>
#include <map>
#include <optional>
#include <vector>

namespace rm_terminal
{

    // 双方现在身上有什么增益:服务器发的 Buff(通常只有自机),加上按规则推的。敌方收到了还没过期的
    // 雷达情报(radar_intel.h)时,用情报里的实际值,不再推。
    // 规则手册 2027 V1.0.0:
    //   5.3.2  读条复活无敌 30 秒,虚弱到踩补给区、基地或前哨站增益点;金币复活虚弱和无敌 3 秒
    //   5.6.3  小能量机关全队和建筑防御 25%,45 秒;大能量机关按平均环数(表 5-22)和激活臂数(表 5-23)
    //   5.6.4  同类增益取最大,防御和易伤相抵
    //   5.7.1  重装前 3 分钟、没装上 42mm 时防御 50%
    //   5.7.2  哨兵姿态(表 5-28)
    //   5.7.3  飞镖命中随机固定目标:对方能量机关增益暂停,时长同遮挡;命中随机移动、末端移动目标:失效
    //   表 5-12 装配三级、四级:全队地面机器人和建筑防御 25% / 50%,到比赛结束
    // 能量机关的事件 3、4 不带是哪一方的:前后 3 秒内我方能量机关变成「已激活」的算我方,否则算敌方。
    // 我方能量机关状态断着时不分,丢掉这条事件。

    enum class BuffTone
    {
        Good,
        Bad,
        Neutral
    };

    struct BuffChip
    {
        QString text;
        BuffTone tone = BuffTone::Neutral;
    };

    struct RuneBuff
    {
        bool big = false;
        int attack_percent = 100; // 规则的写法:200 即伤害 ×2
        int defense_percent = 0;
        int cooling_times = 1; // 热量冷却倍数,1 = 没有
        MonotonicMs started = 0;
        MonotonicMs until = 0;
        bool estimated = false;
        // 终端打开或断线恢复时它已经在生效:不知道哪一秒开的,不写还剩几秒。
        bool timed = true;
        MonotonicMs paused_until = 0;

        bool active(MonotonicMs now) const { return now < until && now >= paused_until; }
    };

    struct SideBuffs
    {
        std::optional<RuneBuff> rune;
        // 基地、前哨站除 100% 以外的防御(100% 看 base_status)。
        int structure_defense = 0;
        QString structure_source;
        bool structure_estimated = false;
    };

    struct SentryPostureInfo
    {
        std::uint32_t posture = 0; // 1 进攻 2 防御 3 移动
        bool powered = false;
        bool weakened = false;
        // 这个姿态本局累计多少秒、强化还能用几秒,从终端打开时数起。
        std::int32_t held_sec = 0;
        std::int32_t powered_left_sec = 0;
        QString effect;
    };

    struct BuffBoard
    {
        // 机器人 id -> 标签,要紧的在前。没有增益的车不在表里。
        std::map<std::uint32_t, std::vector<BuffChip>> chips;
        SideBuffs red, blue;
        bool self_weak = false;
        double self_attack = 1.0;
        std::optional<SentryPostureInfo> sentry;
        // 已经分出是哪一方的能量机关事件(3、4):收到时刻 -> 阵营。战场事件栏据此补上「我方/敌方」。
        std::map<MonotonicMs, std::uint32_t> rune_event_sides;

        const SideBuffs& side(std::uint32_t faction) const { return faction == kBlue ? blue : red; }
    };

    RuneBuff make_rune_buff(bool big, std::uint32_t arms, double rings, MonotonicMs started, bool estimated);

    // 「小 20秒」「大 38秒」,暂停时「暂停 5秒」,已经结束时为空。推算的前面加「约」。
    QString rune_buff_text(const RuneBuff& rune, MonotonicMs now);
    // 只有时间部分:「约40秒」「暂停 5秒」「生效中」,已经结束时为空。compact 写成「约40s」「停5s」,
    // 给战场数据分析的窄格子用(那块面板的秒一律写 s)。
    QString rune_time_text(const RuneBuff& rune, MonotonicMs now, bool compact = false);

    QString sentry_posture_effect(std::uint32_t posture, bool powered, bool weakened);
    // 雷达情报 0x0A05 的哨兵姿态 1–6:「进攻姿态」「强化防御」……认不出时为空。
    QString sentry_posture_name(std::uint32_t posture);
    // 敌方机器人那一栏标题栏的字:「雷达情报」,过期了「雷达情报过期 12秒」(stale 置真),
    // 这一局没收到过时为空。
    QString radar_intel_note(const Snapshot& snapshot, bool* stale = nullptr);

    class BuffTracker
    {
    public:
        BuffBoard update(const Snapshot& snapshot, MonotonicMs now);

    private:
        struct PendingRune
        {
            std::uint32_t kind = 0; // 0:只收到了事件 3
            std::uint32_t arms = 0;
            double rings = 0;
            MonotonicMs at = 0;
            std::vector<MonotonicMs> times;
        };
        struct Revival
        {
            MonotonicMs at = 0;
            bool paid = false;
            // 自机:复活时的累计发射数。虚弱时发射机构锁定,之后发出过弹就一定不虚弱了。
            std::optional<std::uint32_t> fired;
        };

        void reset();
        void onEvent(const EventRecord& event, std::uint32_t ours);
        void onRuneEvent(const EventRecord& event, std::uint32_t ours);
        void rebuildOurRune();
        void trackOurRune(const Snapshot& snapshot, MonotonicMs now, std::uint32_t ours);
        void settlePending(MonotonicMs now, std::uint32_t ours);
        void trackRevivals(const Snapshot& snapshot, MonotonicMs now);
        void trackSentry(const Snapshot& snapshot, MonotonicMs now);

        std::optional<std::uint32_t> round_;
        bool in_match_ = false;
        MonotonicMs last_event_at_ = 0;

        std::vector<PendingRune> pending_;
        std::optional<std::uint32_t> our_status_;
        std::optional<MonotonicMs> status_fresh_since_;
        std::optional<MonotonicMs> our_activated_at_;
        std::optional<std::int32_t> our_activated_elapsed_;
        PendingRune our_info_;
        bool our_kind_from_event_ = false;
        bool our_timed_ = true;
        std::uint32_t our_arms_seen_ = 0;
        double our_rings_seen_ = 0;
        std::optional<RuneBuff> our_rune_;
        std::optional<RuneBuff> their_rune_;
        std::map<MonotonicMs, std::uint32_t> rune_event_sides_;
        int our_fixed_dart_hits_ = 0;
        int their_fixed_dart_hits_ = 0;

        std::map<std::uint32_t, std::uint32_t> last_hp_;
        std::map<std::uint32_t, Revival> revivals_;
        bool self_buffs_seen_ = false;

        std::array<MonotonicMs, 4> held_ms_{};
        std::array<MonotonicMs, 4> powered_ms_{};
        std::optional<MonotonicMs> sentry_last_;
    };

} // namespace rm_terminal
