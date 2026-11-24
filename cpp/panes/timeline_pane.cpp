#include "timeline_pane.h"

#include "theme.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>

namespace rm_terminal {

namespace {

constexpr int kRowHeight = 31;
constexpr int kRowGap = 4;

QString clock(std::int32_t seconds) {
    seconds = std::max(0, seconds);
    return QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

TimelinePane::TimelinePane(QWidget* parent) : QWidget(parent), marks_(match_timeline()) {}

void TimelinePane::setClock(std::optional<std::int32_t> elapsed_sec, std::optional<std::int32_t> remaining_sec) {
    if (elapsed_ == elapsed_sec && remaining_ == remaining_sec) return;
    elapsed_ = elapsed_sec;
    remaining_ = remaining_sec;
    update();
}

QSize TimelinePane::sizeHint() const { return QSize(320, 3 * (kRowHeight + kRowGap)); }

QSize TimelinePane::minimumSizeHint() const { return QSize(160, kRowHeight); }

void TimelinePane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!elapsed_) {
        painter.setFont(theme::labelPx(14));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(10, 0, width() - 10, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("开赛后显示接下来的时间点"));
        return;
    }
    int y = 0;
    bool first = true;
    for (const TimelineMark& mark : marks_) {
        if (mark.at_sec < *elapsed_) continue;
        if (y + kRowHeight > height()) break;
        const QRectF line(0, y, width(), kRowHeight);
        if (first) {
            QLinearGradient glow(line.topLeft(), line.topRight());
            glow.setColorAt(0, QColor(142, 210, 242, 38));
            glow.setColorAt(0.85, QColor(142, 210, 242, 0));
            painter.fillRect(line, glow);
        }
        // 左边:还有几秒;中间色条;右边:那时比赛钟上显示几点,和顶栏计时器对得上。
        painter.setFont(theme::numericPx(15, QFont::DemiBold));
        painter.setPen(first ? theme::kAccentText : theme::kTextBright);
        const int wait = mark.at_sec - *elapsed_;
        painter.drawText(QRectF(10, line.top(), 44, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         wait == 0 ? QStringLiteral("现在") : clock(wait));
        painter.setPen(Qt::NoPen);
        painter.setBrush(mark.kind == AlertKind::Energy ? theme::kCyan : theme::kWarn);
        painter.drawPath(theme::skewBar(QRectF(60, line.center().y() - 7, 3, 14), -14));
        QString text = mark.text;
        text.remove(QStringLiteral("了"));
        painter.setFont(theme::labelPx(14, QFont::Medium));
        painter.setPen(theme::kTextPrimary);
        const QRectF text_box(72, line.top(), width() - 72 - 56, kRowHeight);
        painter.drawText(text_box, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(text_box.width())));
        if (remaining_) {
            painter.setFont(theme::numericPx(13, QFont::DemiBold));
            painter.setPen(theme::kTextMuted);
            painter.drawText(QRectF(width() - 52, line.top(), 44, kRowHeight), Qt::AlignRight | Qt::AlignVCenter,
                             clock(*remaining_ - wait));
        }
        first = false;
        y += kRowHeight + kRowGap;
    }
    if (first) {
        painter.setFont(theme::labelPx(14));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(10, 0, width() - 10, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("这局后面没有固定的时间点了"));
    }
}

}  // namespace rm_terminal
