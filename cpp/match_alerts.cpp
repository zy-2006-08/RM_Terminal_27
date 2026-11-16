#include "match_alerts.h"

#include "presentation.h"
#include "reminder_scheduler.h"

#include <algorithm>
#include <cmath>

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

QString enemy_name(std::uint32_t robot_id) {
    return QStringLiteral("敌方 %1 号%2").arg(display_robot_number(robot_id)).arg(robot_class_name(robot_id));
}

QString intrusion_speech(FieldRoute route) {
    if (route == FieldRoute::Unknown) return QStringLiteral("敌方进入我方半场");
    return QStringLiteral("敌方从%1过来了").arg(route_name(route));
}

std::optional<FieldPoint> fresh_position(const MapRobot& robot) {
    const auto x = fresh(robot.position.x);
    const auto y = fresh(robot.position.y);
    if (!x || !y) return std::nullopt;
    return FieldPoint{*x, *y};
}

// 雷达情报 0x0A04 里我方这边的地形跨越点有没有检测到敌方:bit 1 中央高地上方,bit 3 公路上方,
// bit 13–21 隧道(协议表 1-44)。有就以它为准,比按位置猜的准。
std::optional<FieldRoute> radar_route(const Snapshot& snapshot) {
    if (!snapshot.radar_intel || !snapshot.radar_intel->field_fresh) return std::nullopt;
    const std::uint32_t bits = snapshot.radar_intel->crossings;
    if (bits & (0x1FFu << 13)) return FieldRoute::Tunnel;
    if (bits & (1u << 1)) return FieldRoute::Highland;
    if (bits & (1u << 3)) return FieldRoute::Road;
    return std::nullopt;
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
    case AlertKind::EnemyLowHp: return QStringLiteral("low_hp");
    case AlertKind::BaseAttacked: return QStringLiteral("base");
    case AlertKind::Intrusion: return QStringLiteral("intrusion");
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

std::vector<TimelineMark> match_timeline() {
    std::vector<TimelineMark> out;
    for (const TimelinePoint& point : timeline())
        if (QString::fromUtf8(point.banner).contains(QStringLiteral("可以")) &&
            !QString::fromUtf8(point.banner).contains(QStringLiteral("秒后")))
            out.push_back({point.at_sec, point.kind, QString::fromUtf8(point.banner)});
    return out;
}

QStringList match_alert_phrases() {
    QStringList phrases{QStringLiteral("敌方临时控制我方前哨阵地"),
                        QStringLiteral("我方前哨阵地被敌方完全控制"),
                        QStringLiteral("控制度落后了"),
                        QStringLiteral("敌方在激活我方堡垒"),
                        QStringLiteral("基地被攻击")};
    for (const auto& point : timeline()) phrases << QString::fromUtf8(point.speech);
    for (const std::uint32_t id : {101u, 103u, 104u, 107u}) phrases << low_hp_speech(id);
    for (const FieldRoute route : {FieldRoute::Unknown, FieldRoute::Highland, FieldRoute::Tunnel, FieldRoute::Road})
        phrases << intrusion_speech(route);
    phrases.removeDuplicates();
    return phrases;
}

MatchAlerts::MatchAlerts(MatchAlertSettings settings) : settings_(std::move(settings)) {}

void MatchAlerts::report(std::vector<MatchAlert>& out, MatchAlert alert) const {
    if (alert.kind == AlertKind::Energy && settings_.role == Role::Hero) return;
    if (alert.kind == AlertKind::DartGate && settings_.role != Role::Commander) return;
    if (settings_.disabled.contains(alert_kind_name(alert.kind))) return;
    out.push_back(std::move(alert));
}

void MatchAlerts::resetRound() {
    last_elapsed_.reset();
    last_outpost_.reset();
    last_fortress_.reset();
    last_base_hp_.reset();
    control_armed_ = true;
    low_hp_armed_.clear();
    intruders_.clear();
    last_radar_route_at_.reset();
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
        const bool darts_spent = (red ? snapshot.dart_hits.red : snapshot.dart_hits.blue) >= kDartsPerRound;
        for (const auto& point : timeline()) {
            if (point.kind == AlertKind::DartGate && darts_spent) continue;
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

    // 堡垒(2027):敌方开始激活我方堡垒时提醒一次。激活满 10 秒我方基地护甲展开。
    if (const auto activation =
            fresh(red ? game.red_fortress_activation_sec : game.blue_fortress_activation_sec)) {
        if (*activation > 0 && last_fortress_.value_or(0) == 0) {
            // 规则 5.6.4.9:在激活的那台 100% 易伤,我方基地护甲展开后就没有了。
            const bool thief_exposed = fresh(red ? game.red_base_status : game.blue_base_status) != 2u;
            // 雷达看得到时按位置认出是哪一台(站在我方堡垒增益点上的敌车)。
            QStringList thieves;
            for (const MapRobot& robot : snapshot.map_robots)
                if (robot.faction != settings_.self_faction)
                    if (const auto at = fresh_position(robot); at && zone_at(*at, settings_.self_faction) == FieldZone::Fortress)
                        thieves << enemy_name(robot.id.value);
            QString banner = thieves.isEmpty()
                                 ? (thief_exposed ? QStringLiteral("敌方在激活我方堡垒 · 那台易伤100%")
                                                  : QStringLiteral("敌方在激活我方堡垒"))
                                 : thieves.join(QStringLiteral("、")) + QStringLiteral("在激活我方堡垒") +
                                       (thief_exposed ? QStringLiteral(" · 易伤100%") : QString());
            report(out, {AlertKind::Fortress, AlertLevel::Warning, banner, QStringLiteral("敌方在激活我方堡垒")});
        }
        last_fortress_ = *activation;
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

    // 敌方进入我方半场:看到它从它那半场过了中线才报,同一台 kIntrusionQuietMs 内只报一次。
    // 从哪条路来:雷达情报里我方跨越点有车时以它为准;否则按过中线那一刻的位置,中间是中央高地,
    // 上下两头是隧道;两次位置隔太久(雷达跟丢过)就不写来路。
    const std::uint32_t enemy = red ? 2u : 1u;
    // 跨越点比位置更新得勤(信息波 10Hz,雷达位置最多 5Hz):记住最近一次报的,过中线时往回看几秒。
    if (const auto radar = radar_route(snapshot)) {
        last_radar_route_ = *radar;
        last_radar_route_at_ = now;
    }
    for (const MapRobot& robot : snapshot.map_robots) {
        if (robot.faction != enemy) continue;
        const auto at = fresh_position(robot);
        if (!at) continue;
        Intruder& track = intruders_[robot.id.value];
        if (in_half(*at, enemy, kIntrusionMarginM)) {
            track.inside = false;
            track.last_outside = *at;
            track.last_outside_at = now;
            continue;
        }
        if (track.inside || !in_half(*at, settings_.self_faction, kIntrusionMarginM)) continue;
        track.inside = true;
        if (!track.last_outside_at || (track.last_alert && now - *track.last_alert < kIntrusionQuietMs)) continue;
        track.last_alert = now;
        FieldRoute route = FieldRoute::Unknown;
        if (now - *track.last_outside_at <= kIntrusionTrackMs && std::abs(at->x - track.last_outside.x) > 1e-6) {
            const double t = (kFieldMidX - track.last_outside.x) / (at->x - track.last_outside.x);
            route = route_at_midline(track.last_outside.y + t * (at->y - track.last_outside.y));
        }
        if (last_radar_route_at_ && now - *last_radar_route_at_ <= kIntrusionTrackMs) route = last_radar_route_;
        const QString name = enemy_name(robot.id.value);
        report(out, {AlertKind::Intrusion, AlertLevel::Warning,
                     route == FieldRoute::Unknown ? name + QStringLiteral("进入我方半场")
                                                  : name + QStringLiteral("从%1进入我方半场").arg(route_name(route)),
                     intrusion_speech(route)});
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
