#include "roster_pane.h"

#include "presentation.h"
#include "theme.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace rm_terminal {

namespace {
// 与 qml/RosterPanel.qml 的 rowHeight 上限保持一致,否则两条绘制路径卡片高度不同。
constexpr int kRowHeight = 72;
// 十台必须同时在屏:矮于阈值就改画紧凑版(去兵种行、缩头像),而不是裁掉末尾几台。
constexpr int kRowHeightMin = 40;
constexpr int kRowCompactBelow = 70;
constexpr int kRowGapCompact = 7;
constexpr int kBarHeight = 9;
constexpr int kAvatar = 40;
constexpr int kSkew = 9;
// 分格血条：官方血条是分段的,不是一条连续色块。段数固定,故 HP 变化时
// 段的边界不移动,操作手能靠「掉了几格」估量,而连续条只能靠长度猜。
constexpr int kHpSegments = 12;

QString robot_title(const RosterEntry& entry) {
    return QString::number(display_robot_number(entry.id.value));
}

// 兵种图标：用几何图形而非位图资产 —— 仓库里没有官方头像素材,
// 内联绘制既避免引入无授权图片,也保证任意 DPI 下都清晰。
void paint_avatar(QPainter& painter, const QRect& box, const QString& robot_class,
                  const QColor& tint, bool online) {
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(online ? tint.darker(220) : theme::kSurfaceRaised);
    painter.drawPath(theme::skewPath(QRectF(box), 5));

    const QColor ink = online ? tint.lighter(135) : theme::kOffline;
    painter.setPen(QPen(ink, 1.6));
    painter.setBrush(Qt::NoBrush);
    const QPointF c = QRectF(box).center();

    if (robot_class == QStringLiteral("重装")) {
        painter.drawRect(QRectF(c.x() - 9, c.y() - 6, 18, 12));
        painter.drawLine(QPointF(c.x() - 4, c.y() - 6), QPointF(c.x() - 4, c.y() + 6));
    } else if (robot_class == QStringLiteral("步兵")) {
        painter.drawRect(QRectF(c.x() - 7, c.y() - 5, 14, 10));
        painter.drawLine(QPointF(c.x() + 7, c.y()), QPointF(c.x() + 12, c.y()));
    } else if (robot_class == QStringLiteral("空中")) {
        painter.drawEllipse(c, 4.0, 4.0);
        for (const QPointF& arm : {QPointF(-9, -7), QPointF(9, -7), QPointF(-9, 7), QPointF(9, 7)})
            painter.drawLine(c, c + arm);
    } else if (robot_class == QStringLiteral("哨兵")) {
        painter.drawLine(QPointF(c.x(), c.y() + 8), QPointF(c.x(), c.y() - 2));
        painter.drawEllipse(QPointF(c.x(), c.y() - 5), 5.0, 4.0);
    } else {
        painter.setFont(theme::labelFont(13, true));
        painter.setPen(ink);
        painter.drawText(box, Qt::AlignCenter, QStringLiteral("?"));
    }
    painter.restore();
}

}  // namespace

std::vector<RosterEntry> build_roster(const Snapshot& snapshot, std::uint32_t faction) {
    // 按「已知存在」建行,而非「正在播报位置」:血量来自 RobotHealthSet,与位置无关。
    // 只扫 map_robots 会让有血量无位置的机器人整行消失,操作手会以为对面少一台。
    std::vector<RobotId> ids;
    const auto remember = [&ids](RobotId id) {
        const auto seen = std::find_if(ids.begin(), ids.end(), [&](RobotId candidate) {
            return candidate.value == id.value;
        });
        if (seen == ids.end()) ids.push_back(id);
    };
    for (const MapRobot& robot : snapshot.map_robots)
        if (robot.faction == faction) remember(robot.id);
    for (const RobotHealth& health : snapshot.robot_health)
        if (health.faction == faction) remember(health.id);

    std::vector<RosterEntry> rows;
    for (const RobotId id : ids) {
        RosterEntry entry;
        entry.id = id;
        entry.faction = faction;

        const auto robot = std::find_if(
            snapshot.map_robots.begin(), snapshot.map_robots.end(),
            [&](const MapRobot& candidate) { return candidate.id.value == id.value; });
        if (robot != snapshot.map_robots.end()) {
            entry.is_self = robot->is_self;
            entry.has_position = robot->position.x.value.has_value() &&
                                 robot->position.y.value.has_value();
            entry.position_stale = robot->position.x.freshness == Freshness::Stale ||
                                   robot->position.y.freshness == Freshness::Stale;
        }

        // 全场血量只能来自 RobotHealthSet:RobotDynamicStatus 按协议仅承载自机。
        const auto health = std::find_if(
            snapshot.robot_health.begin(), snapshot.robot_health.end(),
            [&](const RobotHealth& candidate) { return candidate.id.value == id.value; });
        if (health != snapshot.robot_health.end()) {
            entry.hp = health->current_hp.value;
            entry.max_hp = health->max_hp.value;
        }

        const auto found = snapshot.robots.find(id);
        if (found != snapshot.robots.end()) {
            const RobotState& state = found->second;
            // 自机的 RobotDynamicStatus 更细,但缺值时不能把上面的全场血量清掉。
            if (state.dynamic.current_hp.value) entry.hp = state.dynamic.current_hp.value;
            if (state.dynamic.max_hp.value) entry.max_hp = state.dynamic.max_hp.value;
            entry.coin = state.dynamic.coin.value;
            entry.heat = state.dynamic.shooter_heat_17mm.value;
            entry.heat_limit = state.dynamic.shooter_heat_limit.value;
            entry.ammo = state.dynamic.remaining_ammo.value;
            entry.bullet_speed = state.dynamic.bullet_speed.value;
            entry.chassis_power = state.dynamic.chassis_power.value;
            entry.buffer_energy = state.dynamic.buffer_energy.value;
            entry.vision_online = state.telemetry.vision_online.value;
            entry.autoaim_enabled = state.telemetry.autoaim_enabled.value;
            entry.target_locked = state.telemetry.target_locked.value;
            entry.fire_permit = state.telemetry.fire_permit.value;

            std::uint32_t online = 0;
            std::uint32_t total = 0;
            for (const Field<std::uint32_t>* module : {
                     &state.modules.power_manager, &state.modules.rfid,
                     &state.modules.light_strip, &state.modules.small_shooter,
                     &state.modules.big_shooter, &state.modules.uwb,
                     &state.modules.armor, &state.modules.video_transmission,
                     &state.modules.capacitor, &state.modules.main_controller,
                     &state.modules.laser_detection_module}) {
                ++total;
                if (module->value.value_or(0) == 1) ++online;
            }
            if (total > 0 && state.modules.main_controller.value) {
                entry.modules_online = online;
                entry.modules_total = total;
            }
        }

        // 血量决定生存状态,位置新鲜度不参与:一台机器人可能位置过期但仍然活着。
        // 判定放在 dynamic 合并之后,因为自机的 HP 由 RobotDynamicStatus 覆盖。
        if (entry.hp) entry.live = *entry.hp > 0 ? LiveState::Alive : LiveState::Kia;

        rows.push_back(entry);
    }
    std::sort(rows.begin(), rows.end(),
              [](const RosterEntry& a, const RosterEntry& b) { return a.id < b.id; });
    return rows;
}

std::optional<std::uint32_t> self_faction(const Snapshot& snapshot) {
    for (const MapRobot& robot : snapshot.map_robots)
        if (robot.is_self && robot.faction != 0) return robot.faction;
    return std::nullopt;
}

RosterPane::RosterPane(bool mirrored, QWidget* parent)
    : QWidget(parent), mirrored_(mirrored) {
    setMinimumWidth(210);
}

void RosterPane::setEntries(const std::vector<RosterEntry>& entries) {
    entries_ = entries;
    update();
}

QSize RosterPane::sizeHint() const {
    int total = 0;
    for (const RosterEntry& entry : entries_)
        total += kRowHeight + kRowGapCompact;
    return QSize(240, std::max(kRowHeight, total));
}

void RosterPane::paintRow(QPainter& painter, const RosterEntry& entry,
                          const QRect& box) const {
    const QColor faction = entry.faction == 1 ? theme::kRed : theme::kBlue;
    const QColor faction_dim = entry.faction == 1 ? theme::kRedDim : theme::kBlueDim;
    const bool online = entry.has_position && !entry.position_stale;
    const QString robot_class = robot_class_name(entry.id.value);

    const QPainterPath card = theme::skewPathLeading(QRectF(box), kSkew, mirrored_);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(26, 26, 38, 153));
    painter.drawPath(card);

    // 与 RosterRow.qml 的 border.color 同色;自机不单独描色。
    painter.setPen(QPen(QColor(0x33, 0x33, 0x33), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(card);

    // 纵向偏移一律由 box.height() 反推:写死偏移会让矮行的血条画到卡片外面。
    const bool compact = box.height() < kRowCompactBelow;
    const int pad = compact ? 4 : 7;
    const QRect inner = box.adjusted(mirrored_ ? 10 : 10 + kSkew, pad,
                                     mirrored_ ? -(10 + kSkew) : -10, -pad);
    const Qt::Alignment align = mirrored_ ? Qt::AlignRight : Qt::AlignLeft;

    const int avatar_size = std::min(kAvatar, inner.height());
    const QRect avatar = mirrored_
        ? QRect(inner.right() - avatar_size, inner.top(), avatar_size, avatar_size)
        : QRect(inner.left(), inner.top(), avatar_size, avatar_size);
    paint_avatar(painter, avatar, robot_class, faction, online);

    const int text_left = mirrored_ ? inner.left() : avatar.right() + 9;
    const int text_right = mirrored_ ? avatar.left() - 9 : inner.right();
    const QRect text_area(text_left, inner.top(), text_right - text_left, inner.height());

    const int title_h = compact ? 15 : 17;
    painter.setFont(theme::labelFont(compact ? 11 : 12, true));
    painter.setPen(online ? theme::kTextPrimary : theme::kTextMuted);
    painter.drawText(QRect(text_area.left(), text_area.top(), text_area.width(), title_h),
                     align | Qt::AlignVCenter, robot_title(entry));

    int health_top = text_area.top() + title_h;
    if (!compact && !robot_class.isEmpty()) {
        painter.setFont(theme::labelFont(9));
        painter.setPen(online ? faction.lighter(130) : theme::kOffline);
        painter.drawText(QRect(text_area.left(), health_top, text_area.width(), 13),
                         align | Qt::AlignVCenter, robot_class);
        health_top += 15;
    }

    paintHealth(painter, entry,
                QRect(text_area.left(), health_top, text_area.width(),
                      text_area.bottom() - health_top + 1),
                faction, faction_dim);
}

void RosterPane::paintHealth(QPainter& painter, const RosterEntry& entry,
                             const QRect& area, const QColor& faction,
                             const QColor& faction_dim) const {
    const Qt::Alignment align = mirrored_ ? Qt::AlignRight : Qt::AlignLeft;
    const QRect track(area.left(), area.bottom() - kBarHeight, area.width(), kBarHeight);

    if (entry.live == LiveState::NoData || !entry.hp || !entry.max_hp ||
        *entry.max_hp == 0) {
        // 断续刻度 + 状态点,而不是空血条:空条会被读成「血量为 0」,也就是阵亡。
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::kSurfaceRaised);
        const double seg_w = track.width() / static_cast<double>(kHpSegments);
        for (int i = 0; i < kHpSegments; i += 2)
            painter.drawRect(QRectF(track.left() + i * seg_w + 0.6, track.top() + 3.0,
                                    seg_w - 1.2, track.height() - 6.0));

        const QColor dot = entry.has_position
                               ? (entry.position_stale ? theme::kWarn : theme::kOk)
                               : theme::kOffline;
        const int dy = area.top() + 6;
        const int dx = mirrored_ ? area.right() - 5 : area.left();
        painter.setBrush(dot);
        painter.drawEllipse(QPoint(dx + (mirrored_ ? 0 : 2), dy), 3, 3);

        painter.setFont(theme::labelFont(9));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(mirrored_ ? area.left() : area.left() + 12, area.top(),
                               area.width() - 12, 13),
                         align | Qt::AlignVCenter, QStringLiteral("无血量数据"));
        return;
    }

    if (entry.live == LiveState::Kia) {
        painter.setFont(theme::labelFont(11, true));
        painter.setPen(theme::kDanger);
        painter.drawText(QRect(area.left(), area.top(), area.width(), 14),
                         align | Qt::AlignVCenter, QStringLiteral("阵亡  KIA"));

        painter.setFont(theme::numericFont(9));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(area.left(), area.top(), area.width(), 14),
                         (mirrored_ ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                         QStringLiteral("0 / %1").arg(*entry.max_hp));

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::kDanger.darker(260));
        painter.drawRect(track);
        painter.setPen(QPen(theme::kDanger, 1.0));
        painter.drawLine(track.left(), track.center().y(), track.right(),
                         track.center().y());
        return;
    }

    const double ratio = std::clamp(
        static_cast<double>(*entry.hp) / static_cast<double>(*entry.max_hp), 0.0, 1.0);
    // 血条走阵营色,只在危险区改色,与 RosterRow.qml 的 bar.fill 保持同一规则。
    const QColor health = ratio >= 0.5 ? faction : (ratio > 0.25 ? theme::kWarn : theme::kDanger);

    painter.setFont(theme::numericFont(10, true));
    painter.setPen(theme::kTextPrimary);
    painter.drawText(QRect(area.left(), area.top(), area.width(), 14),
                     align | Qt::AlignVCenter,
                     QStringLiteral("%1 / %2").arg(*entry.hp).arg(*entry.max_hp));

    // 百分比而不是经济:经济只有自机有(decoder 按 robot_id 丢弃他机的
    // RobotDynamicStatus),而百分比是扫视时唯一不用做除法就能比较的量。
    painter.setFont(theme::numericFont(9, true));
    painter.setPen(theme::kTextSecondary);
    painter.drawText(QRect(area.left(), area.top(), area.width(), 14),
                     (mirrored_ ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                     QStringLiteral("%1%").arg(static_cast<int>(std::lround(ratio * 100))));

    painter.setPen(Qt::NoPen);
    painter.setBrush(faction_dim.darker(160));
    painter.drawRect(track);

    const int lit = static_cast<int>(std::ceil(ratio * kHpSegments));
    const double seg_w = track.width() / static_cast<double>(kHpSegments);
    for (int i = 0; i < lit; ++i) {
        const int index = mirrored_ ? kHpSegments - 1 - i : i;
        const QRectF seg(track.left() + index * seg_w + 0.6, track.top() + 1.0,
                         seg_w - 1.2, track.height() - 2.0);
        painter.setBrush(i == lit - 1 ? health.lighter(120) : health);
        painter.drawRect(seg);
    }
}

void RosterPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (entries_.empty()) {
        painter.setFont(theme::labelFont(10));
        painter.setPen(theme::kTextMuted);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("无机器人数据"));
        return;
    }

    // 宁可每台矮几像素,也不能让末尾几台被裁掉 —— 少画一台会被读成「对面少一台」。
    const int gaps = kRowGapCompact * static_cast<int>(entries_.size() - 1);
    const int fair = (height() - gaps) / static_cast<int>(entries_.size());
    const int row_h = std::clamp(fair, kRowHeightMin, kRowHeight);

    int y = 0;
    for (const RosterEntry& entry : entries_) {
        const QRect box(0, y, width(), row_h);
        if (box.bottom() > height()) break;
        paintRow(painter, entry, box);
        y += row_h + kRowGapCompact;
    }
}

}  // namespace rm_terminal
