#pragma once

#include "domain.h"
#include "field_zones.h"
#include "seat.h"

#include <QString>
#include <QStringList>

#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace rm_terminal
{

    // 比赛中的事件提醒:前哨阵地易手、控制度落后、敌方在激活我方堡垒、飞镖闸门、能量机关、
    // 敌方残血、我方基地被攻击、敌方进入我方半场(写从哪条路来)。纯逻辑 —— 输入快照,输出这一刻新触发的提醒,不碰界面和
    // 音频,所以「同一次临时控制只提醒一次」这类规则都能在单元测试里逐条钉住。
    //
    // 时间点按《RoboMaster 2027 超级对抗赛比赛规则手册》V1.0.0(20261015):
    //   能量机关 5.6.3:比赛开始和 1:30 各给一次小能量机关机会;3:00、4:15、5:30 各给一次
    //                   大能量机关机会。
    //   飞镖     5.7.3:比赛开始 30 秒后和 4 分钟后各有一次开闸机会。
    // 数据来自 2027 通信协议 V1.0.0 的 GlobalUnitStatus / GlobalSpecialMechanism。
    enum class AlertKind
    {
        OutpostTemp,
        OutpostFull,
        ControlBehind,
        Fortress,
        DartGate,
        Energy,
        EnemyLowHp,
        BaseAttacked,
        Intrusion,
    };

    // 横幅颜色和排队优先级。
    enum class AlertLevel
    {
        Critical,
        Warning,
        Info
    };

    struct MatchAlert
    {
        AlertKind kind;
        AlertLevel level;
        // 横幅文字,可以带数字(「控制度落后 32 分」)。
        QString banner;
        // 语音文字。只用固定句子:语音要赛前合成好,带变化数字的句子没法提前准备。
        QString speech;
    };

    // 配置里用的种类名,与 config.cpp 的 alert_kind_names() 一致。
    QString alert_kind_name(AlertKind kind);
    QString alert_level_name(AlertLevel level);

    // 所有可能说出口的句子,供赛前一次性合成。
    QStringList match_alert_phrases();

    // 赛程里固定的时间点(能量机关、飞镖闸门),按开赛后第几秒排好。「接下来的时间点」面板用。
    struct TimelineMark
    {
        std::int32_t at_sec;
        AlertKind kind;
        QString text;
    };
    std::vector<TimelineMark> match_timeline();

    struct MatchAlertSettings
    {
        std::uint32_t self_faction = 1; // 1 红 2 蓝
        // 能量机关只有步兵能开(规则手册 5.6.3),不提醒重装;飞镖闸门由指挥手开(5.7.3),只提醒指挥手。
        Role role = Role::Infantry;
        int control_gap = 30;
    };

    constexpr double kLowHpRatio = 0.25;
    constexpr double kLowHpRearmRatio = 0.5;
    constexpr MonotonicMs kBaseAttackedQuietMs = 10000;
    // 入侵:离中线这么远才算进了哪一方的半场(雷达在中线附近抖不会来回报);同一台敌车这么久只报一次;
    // 过中线前最后一次在它那半场的位置超过这么久,就不推来路了。
    constexpr double kIntrusionMarginM = 0.3;
    constexpr MonotonicMs kIntrusionQuietMs = 15000;
    constexpr MonotonicMs kIntrusionTrackMs = 3000;
    // 时间点只在刚越过的 5 秒内触发:时钟跳过去(重连、模拟器跳转)时补报几分钟前的
    // 「能量机关可以激活了」只会误导。
    constexpr std::int32_t kTimelineFreshSec = 5;

    class MatchAlerts
    {
    public:
        explicit MatchAlerts(MatchAlertSettings settings);
        std::vector<MatchAlert> observe(const Snapshot& snapshot, MonotonicMs now);

    private:
        void report(std::vector<MatchAlert>& out, MatchAlert alert) const;
        void resetRound();

        MatchAlertSettings settings_;
        bool saw_non_match_ = false;
        bool in_match_ = false;
        std::optional<std::int32_t> last_elapsed_;
        std::optional<std::uint32_t> last_outpost_;
        std::optional<std::int32_t> last_fortress_;
        std::optional<std::uint32_t> last_base_hp_;
        std::optional<MonotonicMs> last_base_alert_;
        bool control_armed_ = true;
        // 每台敌方机器人一个开关:报过残血就关,血量回到一半以上或阵亡后再打开。
        std::map<std::uint32_t, bool> low_hp_armed_;
        struct Intruder
        {
            bool inside = false;
            FieldPoint last_outside;
            std::optional<MonotonicMs> last_outside_at;
            std::optional<MonotonicMs> last_alert;
        };
        std::map<std::uint32_t, Intruder> intruders_;
        FieldRoute last_radar_route_ = FieldRoute::Unknown;
        std::optional<MonotonicMs> last_radar_route_at_;
    };

    // 横幅排队。同时来几条时按级别排(严重的先),同级先来先播;每条显示 kBannerShowMs;
    // 排队超过 kBannerMaxAgeMs 还没轮到的直接丢掉 —— 十几秒前的事再弹出来只会误导。
    constexpr MonotonicMs kBannerShowMs = 4000;
    constexpr MonotonicMs kBannerMaxAgeMs = 10000;
    constexpr std::size_t kBannerMaxQueue = 4;

    struct BannerItem
    {
        QString text;
        AlertLevel level = AlertLevel::Info;
        MonotonicMs queued_at = 0;
    };

    class BannerQueue
    {
    public:
        void push(const QString& text, AlertLevel level, MonotonicMs now);
        std::optional<BannerItem> current(MonotonicMs now);
        // 正在显示的这条是什么时候换上的,横幅底下那道「还剩多久」的细线用。
        MonotonicMs shownAt() const { return shown_at_; }

    private:
        std::deque<BannerItem> queue_;
        std::optional<BannerItem> showing_;
        MonotonicMs shown_at_ = 0;
    };

} // namespace rm_terminal
