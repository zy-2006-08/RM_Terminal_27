#pragma once

#include "domain.h"

#include <QString>
#include <QStringList>

#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace rm_terminal {

// 比赛中的事件提醒:前哨阵地易手、控制度落后、堡垒换人、飞镖闸门、能量机关、加固机会、
// 敌方残血、我方基地被攻击。纯逻辑 —— 输入快照,输出这一刻新触发的提醒,不碰界面和
// 音频,所以「同一次临时控制只提醒一次」这类规则都能在单元测试里逐条钉住。
//
// 时间点按《RoboMaster 2027 超级对抗赛比赛规则手册》V1.0.0(20261015):
//   能量机关 5.6.3:比赛开始和 1:30 各给一次小能量机关机会;3:00、4:15、5:30 各给一次
//                   大能量机关机会。
//   飞镖     5.7.3:比赛开始 30 秒后和 4 分钟后各有一次开闸机会。
//   加固     5.2:  每分钟 +1 次,可累积。
// 2027 通信协议还没发布,阵地/堡垒/加固字段沿用本地模拟词汇,协议出来后在解码器一处改。
enum class AlertKind {
    OutpostTemp,
    OutpostFull,
    ControlBehind,
    Fortress,
    DartGate,
    Energy,
    Reinforce,
    EnemyLowHp,
    BaseAttacked,
};

// 横幅颜色和排队优先级。
enum class AlertLevel { Critical, Warning, Info };

struct MatchAlert {
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

struct MatchAlertSettings {
    std::uint32_t self_faction = 1;  // 1 红 2 蓝
    int control_gap = 30;
    QStringList disabled;
};

constexpr double kLowHpRatio = 0.25;
constexpr double kLowHpRearmRatio = 0.5;
constexpr MonotonicMs kBaseAttackedQuietMs = 10000;
// 时间点只在刚越过的 5 秒内触发:时钟跳过去(重连、模拟器跳转)时补报几分钟前的
// 「能量机关可以激活了」只会误导。
constexpr std::int32_t kTimelineFreshSec = 5;

class MatchAlerts {
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
    std::optional<std::uint32_t> last_fortress_;
    std::optional<std::uint32_t> last_reinforce_;
    std::optional<std::uint32_t> last_base_hp_;
    std::optional<MonotonicMs> last_base_alert_;
    bool control_armed_ = true;
    // 每台敌方机器人一个开关:报过残血就关,血量回到一半以上或阵亡后再打开。
    std::map<std::uint32_t, bool> low_hp_armed_;
};

// 横幅排队。同时来几条时按级别排(严重的先),同级先来先播;每条显示 kBannerShowMs;
// 排队超过 kBannerMaxAgeMs 还没轮到的直接丢掉 —— 十几秒前的事再弹出来只会误导。
constexpr MonotonicMs kBannerShowMs = 4000;
constexpr MonotonicMs kBannerMaxAgeMs = 10000;
constexpr std::size_t kBannerMaxQueue = 4;

struct BannerItem {
    QString text;
    AlertLevel level = AlertLevel::Info;
    MonotonicMs queued_at = 0;
};

class BannerQueue {
public:
    void push(const QString& text, AlertLevel level, MonotonicMs now);
    std::optional<BannerItem> current(MonotonicMs now);

private:
    std::deque<BannerItem> queue_;
    std::optional<BannerItem> showing_;
    MonotonicMs shown_at_ = 0;
};

}  // namespace rm_terminal
