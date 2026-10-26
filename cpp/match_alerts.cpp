#include "match_alerts.h"

#include "presentation.h"
#include "reminder_scheduler.h"

#include <algorithm>

namespace rm_terminal {
namespace {

// 阵地状态:0 未控 1 红临时 2 红完全 3 蓝临时 4 蓝完全(proto 注释)。
constexpr std::uint32_t kRedTemp = 1, kRedFull = 2, kBlueTemp = 3, kBlueFull = 4;

template <class T>
std::optional<T> fresh(const Field<T>& field) {
    if (field.freshness != Freshness::Fresh || !field.value) return std::nullopt;
    return field.value;
}

struct TimelinePoint {
    std::int32_t at_sec;
    AlertKind kind;
    const char* banner;
    const char* speech;
};

const std::vector<TimelinePoint>& timeline() {
    static const std::vector<TimelinePoint> points{
        {0, AlertKind::Energy, "小能量机关可以激活了", "小能量机关可以激活了"},
        {20, AlertKind::DartGate, "飞镖闸门 10 秒后可以打开", "飞镖闸门十秒后可以打开"},
        {30, AlertKind::DartGate, "飞镖闸门可以打开了", "飞镖闸门可以打开了"},
        {90, AlertKind::Energy, "小能量机关可以激活了", "小能量机关可以激活了"},
        {180, AlertKind::Energy, "大能量机关可以激活了", "大能量机关可以激活了"},
        {230, AlertKind::DartGate, "飞镖闸门 10 秒后可以打开", "飞镖闸门十秒后可以打开"},
        {240, AlertKind::DartGate, "飞镖闸门可以打开了", "飞镖闸门可以打开了"},
        {255, AlertKind::Energy, "大能量机关可以激活了", "大能量机关可以激活了"},
        {330, AlertKind::Energy, "大能量机关可以激活了", "大能量机关可以激活了"},
    };
    return points;
}

QString low_hp_speech(std::uint32_t robot_id) {
    return QStringLiteral("敌方%1号%2残血")
        .arg(display_robot_number(robot_id))
        .arg(robot_class_name(robot_id));
}

}  // namespace

QString alert_kind_name(AlertKind kind) {
    switch (kind) {
    case AlertKind::OutpostTemp:
    case AlertKind::OutpostFull: return QStringLiteral("outpost");
    case AlertKind::ControlBehind: return QStringLiteral("control");
    case AlertKind::Fortress: return QStringLiteral("fortress");
    case AlertKind::DartGate: return QStringLiteral("dart");
    case AlertKind::Energy: return QStringLiteral("energy");
    case AlertKind::Reinforce: return QStringLiteral("reinforce");
    case AlertKind::EnemyLowHp: return QStringLiteral("low_hp");
    case AlertKind::BaseAttacked: return QStringLiteral("base");
    }
    return QString();
}

QString alert_level_name(AlertLevel level) {
    switch (level) {
    case AlertLevel::Critical: return QStringLiteral("critical");
    case AlertLevel::Warning: return QStringLiteral("warning");
    case AlertLevel::Info: return QStringLiteral("info");
    }
    return QString();
}

QStringList match_alert_phrases() {
    QStringList phrases{QStringLiteral("敌方临时控制我方前哨阵地"),
                        QStringLiteral("我方前哨阵地被敌方完全控制"),
                        QStringLiteral("控制度落后了"),
                        QStringLiteral("堡垒被敌方占领"),
                        QStringLiteral("我方占领堡垒"),
                        QStringLiteral("堡垒空出来了"),
                        QStringLiteral("有可用的加固机会"),
                        QStringLiteral("基地被攻击")};
    for (const auto& point : timeline()) phrases << QString::fromUtf8(point.speech);
    for (const std::uint32_t id : {101u, 103u, 104u, 106u, 107u}) phrases << low_hp_speech(id);
    phrases.removeDuplicates();
    return phrases;
}

MatchAlerts::MatchAlerts(MatchAlertSettings settings) : settings_(std::move(settings)) {}

void MatchAlerts::report(std::vector<MatchAlert>& out, MatchAlert alert) const {
    if (settings_.disabled.contains(alert_kind_name(alert.kind))) return;
    out.push_back(std::move(alert));
}

void MatchAlerts::resetRound() {
    last_elapsed_.reset();
    last_outpost_.reset();
    last_fortress_.reset();
    last_reinforce_.reset();
    last_base_hp_.reset();
    control_armed_ = true;
    low_hp_armed_.clear();
}

std::vector<MatchAlert> MatchAlerts::observe(const Snapshot& snapshot, MonotonicMs now) {
    std::vector<MatchAlert> out;
    const auto& game = snapshot.game;
    const auto stage = fresh(game.current_stage);
    if (!stage) return out;
    if (*stage != kMatchStage) {
        // 只有亲眼看到过非比赛阶段,下一次进入比赛才算「刚开赛」;进程启动时就在比赛中
        // 的话,不能把已经过去的时间点当成刚发生。
        saw_non_match_ = true;
        if (in_match_) resetRound();
        in_match_ = false;
        return out;
    }
    const bool red = settings_.self_faction != 2;
    if (!in_match_) {
        in_match_ = true;
        if (saw_non_match_) last_elapsed_ = -1;
    }

    // 时间点:能量机关、飞镖闸门。
    if (const auto elapsed = fresh(game.stage_elapsed_sec)) {
        if (!last_elapsed_ || *elapsed < *last_elapsed_) last_elapsed_ = *elapsed;
        for (const auto& point : timeline()) {
            if (*last_elapsed_ < point.at_sec && point.at_sec <= *elapsed &&
                *elapsed - point.at_sec <= kTimelineFreshSec) {
                report(out, {point.kind, AlertLevel::Info, QString::fromUtf8(point.banner),
                           QString::fromUtf8(point.speech)});
            }
        }
        last_elapsed_ = *elapsed;
    }

    // 我方前哨阵地:被临时控制提醒一次,转为完全控制再提醒一次。
    if (const auto zone = fresh(red ? game.red_outpost_zone : game.blue_outpost_zone)) {
        const std::uint32_t enemy_temp = red ? kBlueTemp : kRedTemp;
        const std::uint32_t enemy_full = red ? kBlueFull : kRedFull;
        if (*zone == enemy_temp && last_outpost_ != enemy_temp)
            report(out, {AlertKind::OutpostTemp, AlertLevel::Warning,
                       QStringLiteral("敌方临时控制我方前哨阵地"),
                       QStringLiteral("敌方临时控制我方前哨阵地")});
        if (*zone == enemy_full && last_outpost_ != enemy_full)
            report(out, {AlertKind::OutpostFull, AlertLevel::Critical,
                       QStringLiteral("我方前哨阵地被敌方完全控制"),
                       QStringLiteral("我方前哨阵地被敌方完全控制")});
        last_outpost_ = *zone;
    }

    // 控制度落后:超过阈值提醒一次,追回到阈值一半以内才重新布防,避免在阈值附近反复响。
    const auto mine = fresh(red ? game.red_control_points : game.blue_control_points);
    const auto theirs = fresh(red ? game.blue_control_points : game.red_control_points);
    if (mine && theirs) {
        const long gap = static_cast<long>(*theirs) - static_cast<long>(*mine);
        if (control_armed_ && gap >= settings_.control_gap) {
            report(out, {AlertKind::ControlBehind, AlertLevel::Warning,
                       QStringLiteral("控制度落后 %1 分").arg(gap), QStringLiteral("控制度落后了")});
            control_armed_ = false;
        } else if (!control_armed_ && gap * 2 < settings_.control_gap) {
            control_armed_ = true;
        }
    }

    // 堡垒:0 无人 1 红 2 蓝。
    if (const auto holder = fresh(game.fortress_holder)) {
        const std::uint32_t self = red ? 1u : 2u;
        if (last_fortress_ != *holder) {
            if (*holder != 0 && *holder != self)
                report(out, {AlertKind::Fortress, AlertLevel::Warning, QStringLiteral("堡垒被敌方占领"),
                           QStringLiteral("堡垒被敌方占领")});
            else if (last_fortress_ && *holder == self)
                report(out, {AlertKind::Fortress, AlertLevel::Info, QStringLiteral("我方占领堡垒"),
                           QStringLiteral("我方占领堡垒")});
            else if (last_fortress_ && *last_fortress_ != 0 && *holder == 0)
                report(out, {AlertKind::Fortress, AlertLevel::Info, QStringLiteral("堡垒空出来了"),
                           QStringLiteral("堡垒空出来了")});
        }
        last_fortress_ = *holder;
    }

    // 加固机会:从没有变成有的那一刻提醒;机会累积期间不再重复。
    if (const auto chances =
            fresh(red ? game.red_reinforce_chances : game.blue_reinforce_chances)) {
        if (*chances > 0 && last_reinforce_.value_or(0) == 0)
            report(out, {AlertKind::Reinforce, AlertLevel::Info, QStringLiteral("有可用的加固机会"),
                       QStringLiteral("有可用的加固机会")});
        last_reinforce_ = *chances;
    }

    // 我方基地掉血。10 秒内只报一次:持续被打时每 250ms 报一次等于没有提醒。
    if (const auto base = fresh(red ? game.red_base_hp : game.blue_base_hp)) {
        if (last_base_hp_ && *base < *last_base_hp_ &&
            (!last_base_alert_ || now - *last_base_alert_ >= kBaseAttackedQuietMs)) {
            report(out, {AlertKind::BaseAttacked, AlertLevel::Critical, QStringLiteral("基地被攻击"),
                       QStringLiteral("基地被攻击")});
            last_base_alert_ = now;
        }
        last_base_hp_ = *base;
    }

    // 敌方残血。
    for (const RobotHealth& robot : snapshot.robot_health) {
        if (robot.faction == 0 || robot.faction == (red ? 1u : 2u)) continue;
        const auto hp = fresh(robot.current_hp);
        const auto max = fresh(robot.max_hp);
        if (!hp || !max || *max == 0) continue;
        const double ratio = static_cast<double>(*hp) / static_cast<double>(*max);
        auto armed = low_hp_armed_.try_emplace(robot.id.value, true).first;
        if (*hp == 0 || ratio > kLowHpRearmRatio) {
            armed->second = true;
        } else if (armed->second && ratio <= kLowHpRatio) {
            report(out, {AlertKind::EnemyLowHp, AlertLevel::Warning,
                       QStringLiteral("敌方 %1 号%2残血（%3/%4）")
                           .arg(display_robot_number(robot.id.value))
                           .arg(robot_class_name(robot.id.value))
                           .arg(*hp)
                           .arg(*max),
                       low_hp_speech(robot.id.value)});
            armed->second = false;
        }
    }
    return out;
}

void BannerQueue::push(const QString& text, AlertLevel level, MonotonicMs now) {
    if (showing_ && showing_->text == text) return;
    for (const auto& item : queue_)
        if (item.text == text) return;
    queue_.push_back({text, level, now});
    while (queue_.size() > kBannerMaxQueue) {
        // 挤掉级别最低里最早的那条。
        auto victim = queue_.begin();
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
            if (static_cast<int>(it->level) > static_cast<int>(victim->level)) victim = it;
        queue_.erase(victim);
    }
}

std::optional<BannerItem> BannerQueue::current(MonotonicMs now) {
    if (showing_ && now - shown_at_ < kBannerShowMs) return showing_;
    showing_.reset();
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [now](const BannerItem& item) {
                                    return now - item.queued_at > kBannerMaxAgeMs;
                                }),
                 queue_.end());
    if (queue_.empty()) return std::nullopt;
    auto next = queue_.begin();
    for (auto it = queue_.begin(); it != queue_.end(); ++it)
        if (static_cast<int>(it->level) < static_cast<int>(next->level)) next = it;
    showing_ = *next;
    shown_at_ = now;
    queue_.erase(next);
    return showing_;
}

}  // namespace rm_terminal
