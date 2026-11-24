#include "overview_pane.h"

#include "theme.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace rm_terminal {
namespace {

QColor leader_colour(Leader leader) {
    switch (leader) {
    case Leader::Ours: return theme::kOkText;
    case Leader::Theirs: return theme::kDangerText;
    case Leader::Even: return theme::kWarn;
    case Leader::Unknown: break;
    }
    return theme::kTextSecondary;
}

// 「标签 结论」一段,返回画完以后的右边界。
qreal draw_pair(QPainter& painter, qreal x, qreal y, qreal height, const QString& label, const Verdict& verdict,
                int pixel, qreal max_width) {
    const QFont label_font = theme::labelPx(std::max(11, pixel - 3));
    const QFont value_font = theme::labelPx(pixel, QFont::Bold);
    painter.setFont(label_font);
    painter.setPen(theme::kTextMuted);
    const qreal label_w = QFontMetricsF(label_font).horizontalAdvance(label) + 6;
    painter.drawText(QRectF(x, y, label_w, height), Qt::AlignLeft | Qt::AlignVCenter, label);
    painter.setFont(value_font);
    painter.setPen(leader_colour(verdict.leader));
    const QFontMetricsF metrics(value_font);
    const QString text = metrics.elidedText(verdict.text, Qt::ElideRight, std::max<qreal>(20, max_width - label_w));
    painter.drawText(QRectF(x + label_w, y, max_width - label_w, height), Qt::AlignLeft | Qt::AlignVCenter, text);
    return x + label_w + metrics.horizontalAdvance(text);
}

void draw_line(QPainter& painter, const QRectF& row, const QString& text, const QColor& colour, int pixel,
               int weight = QFont::Medium) {
    const QFont font = theme::labelPx(pixel, weight);
    painter.setFont(font);
    painter.setPen(colour);
    painter.drawText(row, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetricsF(font).elidedText(text, Qt::ElideRight, row.width()));
}

QString clock(std::int64_t seconds) {
    seconds = std::max<std::int64_t>(0, seconds);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

OverviewPane::OverviewPane(QWidget* parent) : QWidget(parent) { setAttribute(Qt::WA_TranslucentBackground); }

void OverviewPane::setOverview(Overview overview) {
    overview_ = std::move(overview);
    update();
}

void OverviewPane::setOwnFaction(std::uint32_t faction) {
    own_faction_ = faction;
    update();
}

QSize OverviewPane::sizeHint() const { return {460, 220}; }

QSize OverviewPane::minimumSizeHint() const { return {240, 90}; }

void OverviewPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(6, 2, -6, -4);
    const qreal line = std::clamp<qreal>(area.height() / 8.0, 18, 30);
    const int pixel = static_cast<int>(std::lround(line * 0.62));
    const Overview& o = overview_;

    if (!o.in_match) {
        draw_line(painter, area, o.now.text, theme::kTextSecondary, pixel + 2);
        return;
    }

    // 第一行:现在结束 | 照这样到 7:00。宽度不够就把第二句放到下一行。
    qreal y = area.top();
    const bool two_rows = area.width() < 620;
    const qreal half = two_rows ? area.width() : area.width() * 0.5;
    draw_pair(painter, area.left(), y, line, QStringLiteral("现在结束"), o.now, pixel + 1, half - 8);
    if (two_rows) y += line;
    draw_pair(painter, two_rows ? area.left() : area.left() + half, y, line,
              QStringLiteral("照这样到 %1").arg(clock(o.match_sec)), o.at_end, pixel + 1, half);
    y += line + 2;

    // 底下的三句从下往上排,剩下的高度给趋势线。
    struct Text { QString text; QColor colour; int weight; };
    std::vector<Text> bottom;
    if (!o.flip.isEmpty())
        bottom.push_back({o.flip, o.flip_urgent ? theme::kDangerText : theme::kCyan, QFont::DemiBold});
    bottom.push_back({o.buffs, theme::kTextBright, QFont::Medium});
    if (!o.next_mark.isEmpty()) bottom.push_back({o.next_mark, theme::kTextSecondary, QFont::Medium});
    const qreal small = line * 0.9;
    const qreal bottom_h = small * static_cast<qreal>(bottom.size());
    const QRectF chart(area.left(), y, area.width(), area.bottom() - bottom_h - y - 2);
    if (chart.height() >= 40) paintTrend(painter, chart);
    qreal ty = area.bottom() - bottom_h;
    for (const Text& text : bottom) {
        draw_line(painter, QRectF(area.left(), ty, area.width(), small), text.text, text.colour, pixel - 1, text.weight);
        ty += small;
    }
}

void OverviewPane::paintTrend(QPainter& painter, const QRectF& area) const {
    const Overview& o = overview_;
    const QColor mine = own_faction_ == 2 ? theme::kFillBlue : theme::kFillRed;
    const QColor theirs = own_faction_ == 2 ? theme::kFillRed : theme::kFillBlue;
    const QFont tick_font = theme::numericPx(11, QFont::Medium);
    const qreal label_w = QFontMetricsF(tick_font).horizontalAdvance(QStringLiteral("+0000")) + 4;
    const QRectF plot = area.adjusted(label_w, 2, -2, -14);

    std::int64_t span = 10;
    for (const auto& [t, diff] : o.trend) span = std::max<std::int64_t>(span, std::llabs(diff));
    std::optional<std::pair<std::int32_t, double>> end;
    if (!o.trend.empty() && o.projection) {
        const double value = o.trend.back().second + *o.projection * (o.match_sec - o.trend.back().first);
        end = std::make_pair(o.match_sec, value);
        span = std::max<std::int64_t>(span, static_cast<std::int64_t>(std::ceil(std::fabs(value))));
    }
    const auto x_of = [&](double t) { return plot.left() + plot.width() * t / std::max(1, o.match_sec); };
    const auto y_of = [&](double v) { return plot.center().y() - plot.height() / 2.0 * v / static_cast<double>(span); };

    painter.setPen(QPen(theme::kBorder, 1));
    painter.setBrush(QColor(255, 255, 255, 6));
    painter.drawRoundedRect(plot, 4, 4);
    painter.setPen(QPen(theme::kTextMuted, 1, Qt::DashLine));
    painter.drawLine(QPointF(plot.left(), plot.center().y()), QPointF(plot.right(), plot.center().y()));

    painter.setFont(tick_font);
    painter.setPen(theme::kTextMuted);
    painter.drawText(QRectF(area.left(), plot.top() - 2, label_w - 4, 14), Qt::AlignRight | Qt::AlignTop,
                     QStringLiteral("+%1").arg(span));
    painter.drawText(QRectF(area.left(), plot.bottom() - 12, label_w - 4, 14), Qt::AlignRight | Qt::AlignBottom,
                     QStringLiteral("-%1").arg(span));
    painter.drawText(QRectF(area.left(), plot.center().y() - 7, label_w - 4, 14), Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("0"));
    painter.drawText(QRectF(plot.left(), plot.bottom(), 60, 14), Qt::AlignLeft | Qt::AlignTop, clock(o.match_sec));
    painter.drawText(QRectF(plot.right() - 60, plot.bottom(), 60, 14), Qt::AlignRight | Qt::AlignTop,
                     QStringLiteral("0:00"));
    painter.drawText(QRectF(plot.left() + 4, plot.top() + 2, plot.width(), 14), Qt::AlignLeft | Qt::AlignTop,
                     QStringLiteral("控制度差 · 线在上面我方领先"));

    if (o.trend.empty()) return;
    QPainterPath line;
    QPainterPath fill;
    fill.moveTo(x_of(o.trend.front().first), plot.center().y());
    for (std::size_t i = 0; i < o.trend.size(); ++i) {
        const QPointF p(x_of(o.trend[i].first), y_of(static_cast<double>(o.trend[i].second)));
        if (i == 0) line.moveTo(p);
        else line.lineTo(p);
        fill.lineTo(p);
    }
    fill.lineTo(x_of(o.trend.back().first), plot.center().y());
    fill.closeSubpath();
    // 中线上面涂我方颜色,下面涂敌方颜色。
    for (const bool upper : {true, false}) {
        painter.save();
        painter.setClipRect(upper ? QRectF(plot.left(), plot.top(), plot.width(), plot.height() / 2)
                                  : QRectF(plot.left(), plot.center().y(), plot.width(), plot.height() / 2));
        QColor tint = upper ? mine : theirs;
        tint.setAlpha(60);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tint);
        painter.drawPath(fill);
        painter.restore();
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(theme::kTextPrimary, 2));
    painter.drawPath(line);
    const QPointF now(x_of(o.trend.back().first), y_of(static_cast<double>(o.trend.back().second)));
    if (end) {
        painter.setPen(QPen(theme::kTextSecondary, 1.6, Qt::DashLine));
        painter.drawLine(now, QPointF(x_of(end->first), y_of(end->second)));
    }
    painter.setPen(QPen(theme::kCyan, 1));
    painter.drawLine(QPointF(now.x(), plot.top()), QPointF(now.x(), plot.bottom()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::kTextPrimary);
    painter.drawEllipse(now, 3.5, 3.5);
}

}  // namespace rm_terminal
