#include "self_status_pane.h"

#include "presentation.h"
#include "theme.h"

#include <QPainter>
#include <QPaintEvent>
#include <algorithm>
#include <cmath>
#include <vector>

namespace rm_terminal {

namespace {

// 按 330 像素宽设计的尺寸。面板更宽、更高时整体放大(最多 1.5 倍),更窄时缩小一点(最少 0.85 倍)。
constexpr int kHeaderHeight = 22;
constexpr int kTileHeight = 52;
constexpr int kTileGap = 6;
constexpr int kGaugeHeight = 30;
constexpr int kPipHeight = 22;
constexpr double kDesignWidth = 330;
// 四段从上到下:标题 / 三个数值格 / 三个仪表 / 四个状态灯。格子矮时从下往上少画几段,不压到别的面板。
constexpr int kContentHeight = kHeaderHeight + 6 + kTileHeight + 8 + kGaugeHeight + 8 + kPipHeight;
constexpr int kMaxExtraGap = 36;

int px(double base, double scale) { return static_cast<int>(std::lround(base * scale)); }

void paint_tile(QPainter& painter, const QRect& box, const QString& label, const QString& value, const QColor& ink,
                double scale) {
    theme::paintGlass(painter, QRectF(box), 6, false, theme::kTile, Qt::transparent);
    const QFont label_font = theme::labelPx(px(12, scale));
    painter.setFont(label_font);
    painter.setPen(theme::kTextSecondary);
    const QRect label_box(box.left() + px(10, scale), box.top() + px(6, scale), box.width() - px(14, scale), px(14, scale));
    painter.drawText(label_box, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(label_font).elidedText(label, Qt::ElideRight, label_box.width()));
    painter.setFont(theme::numericPx(px(22, scale), QFont::DemiBold));
    painter.setPen(ink);
    painter.drawText(QRect(box.left() + px(10, scale), box.top() + px(22, scale), box.width() - px(14, scale), px(26, scale)),
                     Qt::AlignLeft | Qt::AlignVCenter, value);
}

void paint_gauge(QPainter& painter, const QRect& box, const QString& label, const QString& value, double ratio,
                 const QColor& fill, double scale) {
    const QFont value_font = theme::numericPx(px(13, scale), QFont::DemiBold);
    const QFont label_font = theme::labelPx(px(12, scale));
    const int value_w = QFontMetrics(value_font).horizontalAdvance(value);
    const int line = px(14, scale);
    painter.setFont(label_font);
    painter.setPen(theme::kTextSecondary);
    painter.drawText(QRect(box.left(), box.top(), box.width() - value_w - 6, line), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(label_font).elidedText(label, Qt::ElideRight, box.width() - value_w - 6));
    painter.setFont(value_font);
    painter.setPen(theme::kTextBright);
    painter.drawText(QRect(box.left(), box.top(), box.width(), line), Qt::AlignRight | Qt::AlignVCenter, value);
    const QRectF track(box.left() + 3, box.top() + px(20, scale), box.width() - 6, std::max(4, px(6, scale)));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 18));
    painter.drawPath(theme::skewBar(track, -24));
    QColor glow = fill;
    glow.setAlphaF(0.88);
    painter.setBrush(glow);
    painter.drawPath(theme::skewBar(QRectF(track.left(), track.top(), track.width() * std::clamp(ratio, 0.0, 1.0),
                                           track.height()),
                                    -24));
}

// 状态灯画成「圆点 + 字」,不画成按钮:画成按钮的样子,操作手会去点「可远程补血」。补血补弹按 H、O、I。
void paint_pip(QPainter& painter, const QRect& box, const QString& text, std::optional<bool> on,
               const QColor& on_colour, double scale) {
    const bool lit = on.value_or(false);
    const qreal d = std::max(6.0, 8.0 * scale);
    const QRectF dot(box.left() + 3, box.center().y() + 0.5 - d / 2, d, d);
    if (lit) {
        QColor glow = on_colour;
        glow.setAlpha(70);
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(dot.adjusted(-2.5, -2.5, 2.5, 2.5));
        painter.setBrush(on_colour);
        painter.drawEllipse(dot);
    } else {
        painter.setPen(QPen(on.has_value() ? theme::kTextSecondary : theme::kTextMuted, 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(dot.adjusted(0.6, 0.6, -0.6, -0.6));
    }
    const QFont font = theme::labelPx(px(12, scale), lit ? QFont::DemiBold : QFont::Normal);
    painter.setFont(font);
    painter.setPen(!on.has_value() ? theme::kTextMuted : (lit ? theme::kTextPrimary : theme::kTextSecondary));
    const int text_left = static_cast<int>(std::ceil(dot.right())) + 6;
    const QRect text_box(text_left, box.top(), box.right() - text_left, box.height());
    painter.drawText(text_box, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(font).elidedText(text, Qt::ElideRight, text_box.width()));
}

QString uint_text(std::optional<std::uint32_t> v) { return v ? QString::number(*v) : QStringLiteral("--"); }

QString double_text(std::optional<double> v, int precision) {
    return v ? QStringLiteral("%1").arg(*v, 0, 'f', precision) : QStringLiteral("--");
}

}  // namespace

SelfStatusPane::SelfStatusPane(QWidget* parent) : QWidget(parent) {}

void SelfStatusPane::setEntry(const std::optional<RosterEntry>& entry) {
    entry_ = entry;
    update();
}

QSize SelfStatusPane::sizeHint() const { return QSize(420, kContentHeight); }

void SelfStatusPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (!entry_) {
        painter.setFont(theme::labelPx(14));
        painter.setPen(theme::kTextMuted);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待本机数据"));
        return;
    }

    const RosterEntry& e = *entry_;
    const int w = width();
    const int h = height();
    // 大小:按宽度放大缩小,高度也够才放大;高度够时三个仪表竖着排,每个占一整行。
    // 剩下的高度匀到各段之间,不全堆在下面。
    const double base_row = kContentHeight;
    const double scale = std::clamp(std::min(w / kDesignWidth, h / base_row), 0.85, 1.5);
    const auto height_of = [&](bool stacked) {
        const int gauges = stacked ? 3 * px(kGaugeHeight, scale) + 2 * 8 : px(kGaugeHeight, scale);
        return px(kHeaderHeight, scale) + 6 + px(kTileHeight, scale) + 8 + gauges + 8 + px(kPipHeight, scale);
    };
    const bool stacked = h >= height_of(true) + 12 || (w < 300 && h >= height_of(true));
    const int sections = stacked ? 6 : 4;
    // 段与段之间每道最多多给 kMaxExtraGap;再多出来的上下平分,整块居中,不全空在下面。
    const int spare = std::max(0, h - height_of(stacked));
    const int extra = std::min(spare / (sections + 1), kMaxExtraGap);
    const int top = (spare - extra * (sections - 1)) / 2;

    const int header_h = px(kHeaderHeight, scale);
    QFont title = theme::labelPx(px(15, scale), QFont::Bold);
    const QString hp_text = e.hp && e.max_hp && *e.max_hp > 0 ? QStringLiteral("%1 / %2").arg(*e.hp).arg(*e.max_hp)
                                                               : QString();
    const QFont hp_font = theme::numericPx(px(21, scale), QFont::Bold);
    const int hp_w = hp_text.isEmpty() ? 0 : QFontMetrics(hp_font).horizontalAdvance(hp_text) + 8;
    int y = top;
    painter.setFont(title);
    painter.setPen(theme::kTextPrimary);
    const QString name = e.level ? QStringLiteral("本机 %1 号%2 · %3 级")
                                       .arg(display_robot_number(e.id.value))
                                       .arg(robot_class_name(e.id.value))
                                       .arg(*e.level)
                                 : QStringLiteral("本机 %1 号%2")
                                       .arg(display_robot_number(e.id.value))
                                       .arg(robot_class_name(e.id.value));
    painter.drawText(QRect(0, y, w - hp_w, header_h), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(title).elidedText(name, Qt::ElideRight, w - hp_w));
    if (!hp_text.isEmpty()) {
        const double ratio = static_cast<double>(*e.hp) / static_cast<double>(*e.max_hp);
        painter.setFont(hp_font);
        painter.setPen(ratio > 0.25 ? theme::kTextPrimary : theme::kDangerText);
        painter.drawText(QRect(0, y, w, header_h), Qt::AlignRight | Qt::AlignVCenter, hp_text);
    }

    const int cols = 3;
    const int tile_w = (w - kTileGap * (cols - 1)) / cols;
    const int tile_h = px(kTileHeight, scale);
    y += header_h + 6 + extra;
    if (y + tile_h > h) return;
    paint_tile(painter, QRect(0, y, tile_w, tile_h), QStringLiteral("允许发弹量"), uint_text(e.ammo),
               e.ammo && *e.ammo == 0 ? theme::kWarn : theme::kTextPrimary, scale);
    paint_tile(painter, QRect(tile_w + kTileGap, y, tile_w, tile_h), QStringLiteral("弹速 m/s"),
               double_text(e.bullet_speed, 1), theme::kTextPrimary, scale);
    paint_tile(painter, QRect((tile_w + kTileGap) * 2, y, tile_w, tile_h), QStringLiteral("我方经济"),
               uint_text(e.economy), theme::kTextPrimary, scale);

    struct Gauge {
        QString label;
        QString value;
        double ratio;
        QColor fill;
    };
    std::vector<Gauge> gauges;
    // 热量的危险方向和血量相反,所以取 1-ratio 喂给 healthColor:热量越接近上限越该显红。
    if (e.heat && e.heat_limit && *e.heat_limit > 0) {
        const double ratio = *e.heat / static_cast<double>(*e.heat_limit);
        gauges.push_back({QStringLiteral("枪口热量"), QStringLiteral("%1 / %2").arg(*e.heat, 0, 'f', 0).arg(*e.heat_limit),
                          ratio, theme::healthColor(1.0 - ratio)});
    } else {
        gauges.push_back({QStringLiteral("枪口热量"), QStringLiteral("--"), 0.0, theme::kOffline});
    }
    if (e.buffer_energy && e.buffer_limit && *e.buffer_limit > 0)
        gauges.push_back({QStringLiteral("缓冲能量"), QStringLiteral("%1 / %2").arg(*e.buffer_energy).arg(*e.buffer_limit),
                          static_cast<double>(*e.buffer_energy) / *e.buffer_limit, theme::kCyan});
    else
        gauges.push_back({QStringLiteral("缓冲能量"), uint_text(e.buffer_energy), 0.0, theme::kOffline});
    if (e.modules_online && e.modules_total)
        gauges.push_back({QStringLiteral("模块在线"), QStringLiteral("%1 / %2").arg(*e.modules_online).arg(*e.modules_total),
                          static_cast<double>(*e.modules_online) / static_cast<double>(*e.modules_total),
                          *e.modules_online == *e.modules_total ? theme::kOk : theme::kWarn});
    else
        gauges.push_back({QStringLiteral("模块在线"), QStringLiteral("--"), 0.0, theme::kOffline});

    const int gauge_h = px(kGaugeHeight, scale);
    y += tile_h + 8 + extra;
    if (stacked) {
        for (std::size_t i = 0; i < gauges.size(); ++i) {
            if (y + gauge_h > h) return;
            const Gauge& g = gauges[i];
            paint_gauge(painter, QRect(0, y, w, gauge_h), g.label, g.value, g.ratio, g.fill, scale);
            if (i + 1 < gauges.size()) y += gauge_h + 8 + extra;
        }
    } else {
        if (y + gauge_h > h) return;
        const int gauge_w = (w - kTileGap * 2 * 2) / 3;
        for (std::size_t i = 0; i < gauges.size(); ++i)
            paint_gauge(painter, QRect(static_cast<int>(i) * (gauge_w + kTileGap * 2), y, gauge_w, gauge_h), gauges[i].label,
                        gauges[i].value, gauges[i].ratio, gauges[i].fill, scale);
    }

    const int pip_h = px(kPipHeight, scale);
    y += gauge_h + 8 + extra;
    if (y + pip_h > h) return;
    const int pip_w = (w - 3 * 6) / 4;
    const struct {
        QString text;
        std::optional<bool> on;
        QColor colour;
    } pips[] = {
        {QStringLiteral("脱战"), e.out_of_combat, theme::kOk},
        {QStringLiteral("可远程补血"), e.can_remote_heal, theme::kCyan},
        {QStringLiteral("可远程补弹"), e.can_remote_ammo, theme::kCyan},
        {QStringLiteral("待复活"), e.pending_respawn, theme::kWarn},
    };
    for (int i = 0; i < 4; ++i)
        paint_pip(painter, QRect(i * (pip_w + 6), y, pip_w, pip_h), pips[i].text, pips[i].on, pips[i].colour, scale);
}

}  // namespace rm_terminal
