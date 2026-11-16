#include "event_feed_pane.h"

#include "theme.h"

#include <QFontMetrics>
#include <QHash>
#include <QPainter>
#include <QPaintEvent>
#include <QLinearGradient>
#include <QRegularExpression>
#include <cmath>
#include <vector>

namespace rm_terminal {

namespace {

constexpr int kRowHeight = 31;
constexpr int kRowGap = 4;
constexpr int kTimeLeft = 10;
constexpr int kMarkGap = 8;

QFont row_font() { return theme::labelPx(14, QFont::Medium); }

QColor mark_colour(EventBanner banner) {
    switch (banner) {
        case EventBanner::Red: return theme::kRed;
        case EventBanner::Blue: return theme::kBlue;
        case EventBanner::Positive: return theme::kOkText;
        case EventBanner::Neutral: break;
    }
    return theme::kTextMuted;
}

// 一行事件拆成几段:「红方4号步兵」「蓝方3号步兵」这种名字按阵营上色,其余正文白色,
// 正文里的数字(「46/200」)换数字字体。
struct Run {
    QString text;
    QColor colour;
};

std::vector<Run> runs_of(const QString& text) {
    static const QRegularExpression names(QStringLiteral("(红方|蓝方)[0-9]*号?[^\\s，,。：:]*"));
    std::vector<Run> runs;
    int at = 0;
    auto it = names.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedStart() > at) runs.push_back({text.mid(at, match.capturedStart() - at), theme::kTextPrimary});
        const bool blue = match.captured(1) == QStringLiteral("蓝方");
        runs.push_back({match.captured(0), blue ? theme::kBlueText : theme::kRedText});
        at = match.capturedEnd();
    }
    if (at < text.size()) runs.push_back({text.mid(at), theme::kTextPrimary});
    return runs;
}

}  // namespace

QFont event_time_font() { return theme::numericPx(13, QFont::DemiBold); }

// 能量机关(Event 3/4)按事件语义走绿色,与激活方无关:红方激活也不该渲染成红色警示横幅。
EventBanner event_banner(const EventRecord& record) {
    if (record.event_id == 3 || record.event_id == 4) return EventBanner::Positive;
    switch (record.faction) {
        case 1: return EventBanner::Red;
        case 2: return EventBanner::Blue;
        default: break;
    }
    return EventBanner::Neutral;
}

QString event_banner_asset(EventBanner banner) {
    switch (banner) {
        case EventBanner::Red: return QStringLiteral(":/images/message/message_red.png");
        case EventBanner::Blue: return QStringLiteral(":/images/message/message_blue.png");
        case EventBanner::Positive:
            return QStringLiteral(":/images/message/message_green.png");
        case EventBanner::Neutral: break;
    }
    return QString();
}

std::vector<EventFeedRow> build_event_rows(const Snapshot& snapshot, std::size_t max,
                                           const std::map<MonotonicMs, std::uint32_t>* rune_sides) {
    const std::vector<EventRecord> records = snapshot.events.recent(max);
    std::vector<EventFeedRow> rows;
    rows.reserve(records.size());
    for (const EventRecord& record : records) {
        // 时间列是收到事件时的比赛倒计时,和顶栏时钟同一种读法;比赛外收到的显示 --。
        const QString time =
            record.countdown_sec
                ? QStringLiteral("%1:%2")
                      .arg(*record.countdown_sec / 60, 2, 10, QLatin1Char('0'))
                      .arg(*record.countdown_sec % 60, 2, 10, QLatin1Char('0'))
                : QStringLiteral("--");
        QString text = QString::fromStdString(record.text);
        EventBanner banner = event_banner(record);
        // 协议的能量机关事件不带是哪一方的;增益推算分出来以后补上,敌方的按敌方配色。
        if (rune_sides && snapshot.self && (record.event_id == 3 || record.event_id == 4))
            if (const auto side = rune_sides->find(record.received_at); side != rune_sides->end()) {
                const bool ours = side->second == faction_of(snapshot.self->value);
                text = (ours ? QStringLiteral("我方") : QStringLiteral("敌方")) + text +
                       (ours ? QString() : QStringLiteral("（推算）"));
                if (!ours) banner = side->second == kBlue ? EventBanner::Blue : EventBanner::Red;
            }
        rows.push_back({time, text, banner});
    }
    return rows;
}

EventFeedPane::EventFeedPane(QWidget* parent) : QWidget(parent) { setFont(row_font()); }

void EventFeedPane::setRows(const std::vector<EventFeedRow>& rows) {
    rows_ = rows;
    update();
}

void EventFeedPane::setDroppedCount(std::size_t dropped) {
    dropped_ = dropped;
    update();
}

int EventFeedPane::rowHeight() { return kRowHeight; }

// 宽度提示必须减掉所在卡片的左右内边距。报满花名册列宽时,卡片会在提示之外再加边距,
// 把这一列顶宽。事件文字过长由省略号收尾,不靠宽提示撑开。
QSize EventFeedPane::sizeHint() const {
    return QSize(kRosterColumnWidth - kCardHorizontalMargins, rowHeight());
}

void EventFeedPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (rows_.empty() && dropped_ == 0) {
        painter.setFont(row_font());
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(kTimeLeft, 0, width() - kTimeLeft, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("暂无赛事事件"));
        return;
    }

    const QFont text_font = row_font();
    const QFont number_font = theme::numericPx(15, QFont::DemiBold);
    const QFontMetricsF text_metrics(text_font);
    int y = 0;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const EventFeedRow& row = rows_[index];
        if (y + kRowHeight > height()) break;
        const QRectF line(0, y, width(), kRowHeight);
        const bool newest = index == 0;
        // 最新一条垫一层淡点缀色,一眼看出刚发生的是哪条。
        if (newest) {
            QLinearGradient glow(line.topLeft(), line.topRight());
            glow.setColorAt(0, QColor(142, 210, 242, 38));
            glow.setColorAt(0.85, QColor(142, 210, 242, 0));
            painter.fillRect(line, glow);
        }
        painter.setFont(event_time_font());
        painter.setPen(newest ? theme::kTextBright : theme::kTextMuted);
        painter.drawText(QRectF(kTimeLeft, line.top(), kEventTimeWidth, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         row.time);

        const qreal mark_x = kTimeLeft + kEventTimeWidth + kMarkGap;
        painter.setPen(Qt::NoPen);
        painter.setBrush(mark_colour(row.banner));
        painter.drawPath(theme::skewBar(QRectF(mark_x, line.center().y() - 7, 3, 14), -14));

        qreal x = mark_x + 3 + kMarkGap;
        const qreal right = line.right() - 8;
        const qreal ellipsis = text_metrics.horizontalAdvance(QStringLiteral("…"));
        bool clipped = false;
        for (const Run& run : runs_of(row.text)) {
            const qreal width = theme::mixedTextWidth(run.text, text_font, number_font);
            if (x + width > right) {
                // 放不下就按字截断,末尾补省略号。
                QString cut = run.text;
                while (!cut.isEmpty() && x + theme::mixedTextWidth(cut, text_font, number_font) + ellipsis > right)
                    cut.chop(1);
                if (!cut.isEmpty())
                    x += theme::paintMixedText(painter, QRectF(x, line.top(), right - x, kRowHeight),
                                               Qt::AlignLeft | Qt::AlignVCenter, cut, text_font, number_font, run.colour);
                clipped = true;
                break;
            }
            x += theme::paintMixedText(painter, QRectF(x, line.top(), width + 1, kRowHeight),
                                       Qt::AlignLeft | Qt::AlignVCenter, run.text, text_font, number_font, run.colour);
        }
        if (clipped) {
            painter.setFont(text_font);
            painter.setPen(theme::kTextSecondary);
            painter.drawText(QRectF(x, line.top(), ellipsis + 2, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("…"));
        }
        y += kRowHeight + kRowGap;
    }

    if (dropped_ > 0 && y + kRowHeight <= height()) {
        painter.setFont(theme::labelPx(13));
        painter.setPen(theme::kTextMuted);
        const qreal left = kTimeLeft + kEventTimeWidth + kMarkGap + 3 + kMarkGap;
        painter.drawText(QRectF(left, y, width() - left, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("… 更早 %1 条已滚出缓冲").arg(dropped_));
    }
}

}  // namespace rm_terminal
