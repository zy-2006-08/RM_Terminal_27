#include "self_status_pane.h"

#include "presentation.h"
#include "theme.h"

#include <QPainter>
#include <QPaintEvent>
#include <algorithm>
#include <cmath>

namespace rm_terminal {

namespace {

constexpr int kHeaderHeight = 20;
constexpr int kTileHeight = 38;
constexpr int kTileGap = 6;
constexpr int kBarHeight = 9;
constexpr int kGaugeHeight = 26;
constexpr int kPipHeight = 16;
// 三段纵向内容:数值格 / 仪表 / 状态灯。写死高度会让面板在 720p 上把最小高度
// 堆到超出窗口(实测 766 > 720),所以最小高度按内容反推 —— 改行数时只改这里。
constexpr int kContentHeight =
    kHeaderHeight + 2 + kTileHeight + 5 + kGaugeHeight + 5 + kPipHeight;

void paint_tile(QPainter& painter, const QRect& box, const QString& label,
                const QString& value, const QColor& ink) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::kSurfaceRaised);
    painter.drawPath(theme::skewPath(QRectF(box), 6));

    painter.setFont(theme::labelFont(9));
    painter.setPen(theme::kTextMuted);
    painter.drawText(QRect(box.left() + 9, box.top() + 4, box.width() - 14, 13),
                     Qt::AlignLeft | Qt::AlignVCenter, label);

    painter.setFont(theme::numericFont(14, true));
    painter.setPen(ink);
    painter.drawText(QRect(box.left() + 9, box.top() + 17, box.width() - 18, 19),
                     Qt::AlignLeft | Qt::AlignVCenter, value);
}

void paint_gauge(QPainter& painter, const QRect& box, const QString& label,
                 const QString& value, double ratio, const QColor& fill) {
    painter.setFont(theme::labelFont(9));
    painter.setPen(theme::kTextMuted);
    painter.drawText(QRect(box.left(), box.top(), box.width(), 13),
                     Qt::AlignLeft | Qt::AlignVCenter, label);
    painter.setFont(theme::numericFont(10, true));
    painter.setPen(theme::kTextSecondary);
    painter.drawText(QRect(box.left(), box.top(), box.width(), 13),
                     Qt::AlignRight | Qt::AlignVCenter, value);

    const QRect track(box.left(), box.top() + 16, box.width(), kBarHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::kSurfaceRaised);
    painter.drawRect(track);
    painter.setBrush(fill);
    painter.drawRect(QRect(track.left(), track.top(),
                           static_cast<int>(track.width() * std::clamp(ratio, 0.0, 1.0)),
                           track.height()));
}

void paint_pip(QPainter& painter, const QRect& box, const QString& text,
               std::optional<bool> on, const QColor& on_colour) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(on.has_value() && *on ? on_colour.darker(150) : theme::kSurfaceRaised);
    painter.drawPath(theme::skewPath(QRectF(box), 4));
    if (!on.has_value()) {
        painter.setPen(QPen(theme::kBorder, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(theme::skewPath(QRectF(box), 4));
    }
    // 不用 on_colour.lighter():会在绘制层撤销 theme 里的降饱和,变回荧光绿/黄。
    painter.setFont(theme::labelFont(9, true));
    painter.setPen(!on.has_value() ? theme::kTextMuted
                                   : (*on ? theme::kTextPrimary : theme::kOffline));
    painter.drawText(box, Qt::AlignCenter, text);
}

QString uint_text(std::optional<std::uint32_t> v) {
    return v ? QString::number(*v) : QStringLiteral("--");
}

QString double_text(std::optional<double> v, int precision) {
    return v ? QStringLiteral("%1").arg(*v, 0, 'f', precision) : QStringLiteral("--");
}

}  // namespace

SelfStatusPane::SelfStatusPane(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(kContentHeight);
}

void SelfStatusPane::setEntry(const std::optional<RosterEntry>& entry) {
    entry_ = entry;
    update();
}

QSize SelfStatusPane::sizeHint() const { return QSize(420, kContentHeight); }

void SelfStatusPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (!entry_) {
        painter.setFont(theme::labelFont(10));
        painter.setPen(theme::kTextMuted);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待自机数据"));
        return;
    }

    const RosterEntry& e = *entry_;
    const int width_avail = width();

    painter.setFont(theme::labelFont(11, true));
    painter.setPen(theme::kCyan);
    painter.drawText(QRect(0, 0, width_avail, kHeaderHeight),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     e.level ? QStringLiteral("自机 %1 · %2 · %3 级")
                                   .arg(display_robot_number(e.id.value))
                                   .arg(robot_class_name(e.id.value))
                                   .arg(*e.level)
                             : QStringLiteral("自机 %1 · %2")
                                   .arg(display_robot_number(e.id.value))
                                   .arg(robot_class_name(e.id.value)));

    if (e.hp && e.max_hp && *e.max_hp > 0) {
        const double ratio = static_cast<double>(*e.hp) / static_cast<double>(*e.max_hp);
        painter.setFont(theme::numericFont(12, true));
        painter.setPen(ratio > 0.25 ? theme::kTextPrimary : theme::kDanger);
        painter.drawText(QRect(0, 0, width_avail, kHeaderHeight),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1 / %2").arg(*e.hp).arg(*e.max_hp));
    }

    const int cols = 3;
    const int tile_w = (width_avail - kTileGap * (cols - 1)) / cols;
    const int tile_y = kHeaderHeight + 2;
    paint_tile(painter, QRect(0, tile_y, tile_w, kTileHeight), QStringLiteral("弹速 m/s"),
               double_text(e.bullet_speed, 2), theme::kTextPrimary);
    paint_tile(painter, QRect(tile_w + kTileGap, tile_y, tile_w, kTileHeight),
               QStringLiteral("允许发弹量"), uint_text(e.ammo), theme::kTextPrimary);
    paint_tile(painter, QRect((tile_w + kTileGap) * 2, tile_y, tile_w, kTileHeight),
               QStringLiteral("我方经济"), uint_text(e.economy), theme::kTextPrimary);

    const int gauge_w = (width_avail - kTileGap * 2) / 3;
    int y = tile_y + kTileHeight + 5;

    // 热量的危险方向和血量相反,所以取 1-ratio 喂给 healthColor:热量越接近上限
    // 越该显红,而 healthColor 的语义是「值越高越安全」。
    if (e.heat && e.heat_limit && *e.heat_limit > 0) {
        const double ratio = *e.heat / static_cast<double>(*e.heat_limit);
        paint_gauge(painter, QRect(0, y, gauge_w, kGaugeHeight), QStringLiteral("枪口热量"),
                    QStringLiteral("%1 / %2").arg(*e.heat, 0, 'f', 0).arg(*e.heat_limit), ratio,
                    theme::healthColor(1.0 - ratio));
    } else {
        paint_gauge(painter, QRect(0, y, gauge_w, kGaugeHeight), QStringLiteral("枪口热量"),
                    QStringLiteral("--"), 0.0, theme::kOffline);
    }

    if (e.buffer_energy && e.buffer_limit && *e.buffer_limit > 0) {
        paint_gauge(painter, QRect(gauge_w + kTileGap, y, gauge_w, kGaugeHeight),
                    QStringLiteral("缓冲能量"),
                    QStringLiteral("%1 / %2").arg(*e.buffer_energy).arg(*e.buffer_limit),
                    static_cast<double>(*e.buffer_energy) / *e.buffer_limit, theme::kCyan);
    } else {
        paint_gauge(painter, QRect(gauge_w + kTileGap, y, gauge_w, kGaugeHeight),
                    QStringLiteral("缓冲能量"), uint_text(e.buffer_energy), 0.0, theme::kOffline);
    }

    if (e.modules_online && e.modules_total) {
        paint_gauge(painter, QRect((gauge_w + kTileGap) * 2, y, gauge_w, kGaugeHeight),
                    QStringLiteral("模块在线"),
                    QStringLiteral("%1 / %2").arg(*e.modules_online).arg(*e.modules_total),
                    static_cast<double>(*e.modules_online) /
                        static_cast<double>(*e.modules_total),
                    *e.modules_online == *e.modules_total ? theme::kOk : theme::kWarn);
    } else {
        paint_gauge(painter, QRect((gauge_w + kTileGap) * 2, y, gauge_w, kGaugeHeight),
                    QStringLiteral("模块在线"), QStringLiteral("--"), 0.0, theme::kOffline);
    }

    y += kGaugeHeight + 5;
    const int pip_w = (width_avail - 3 * 6) / 4;
    const struct { QString text; std::optional<bool> on; QColor colour; } pips[] = {
        {QStringLiteral("脱战"), e.out_of_combat, theme::kOk},
        {QStringLiteral("可远程补血"), e.can_remote_heal, theme::kCyan},
        {QStringLiteral("可远程补弹"), e.can_remote_ammo, theme::kCyan},
        {QStringLiteral("待复活"), e.pending_respawn, theme::kWarn},
    };
    for (int i = 0; i < 4; ++i)
        paint_pip(painter, QRect(i * (pip_w + 6), y, pip_w, 18), pips[i].text, pips[i].on,
                  pips[i].colour);
}

}  // namespace rm_terminal
