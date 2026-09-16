#include "battle_analysis_pane.h"

#include "theme.h"

#include <QPainter>
#include <QPaintEvent>
#include <algorithm>
#include <cmath>

namespace rm_terminal {

namespace {

constexpr int kBannerHeight = 34;
constexpr int kBannerGap = 6;
constexpr int kCardGap = 6;
constexpr int kCardHeight = 62;
constexpr int kMetricCount = 3;
// 最小高度按内容反推,和 self_status_pane 同样的理由:写死高度会让面板把窗口最小
// 高度顶到 720 以上,dashboard_layout 的 720p 断言就是守这个。改布局只改这里。
constexpr int kContentHeight = kBannerHeight + kBannerGap + kCardHeight;

QColor stance_colour(MetricStance stance) {
    switch (stance) {
        case MetricStance::Ahead: return theme::kOk;
        case MetricStance::Behind: return theme::kDanger;
        case MetricStance::Even: return theme::kTextSecondary;
        case MetricStance::Unknown: break;
    }
    return theme::kTextMuted;
}

QColor hold_colour(FortressHold hold) {
    switch (hold) {
        case FortressHold::Ours: return theme::kOk;
        case FortressHold::Theirs: return theme::kDanger;
        case FortressHold::Neutral: return theme::kTextSecondary;
        case FortressHold::Unknown: break;
    }
    return theme::kTextMuted;
}

QString diff_text(const AnalysisMetric& metric) {
    const std::optional<std::int64_t> diff = metric_diff(metric);
    if (!diff) return QStringLiteral("--");
    if (*diff > 0) return QStringLiteral("+%1").arg(*diff);
    return QString::number(*diff);
}

// 优劣条:以中线为原点,按 self/(self+enemy) 向左右伸出。两侧和为 0 或任一侧缺数据
// 时只画中线,否则 0/0 的除法会被画成满条,读成「压倒性领先」。
void paint_balance(QPainter& painter, const QRect& track, const AnalysisMetric& metric,
                   MetricStance stance) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::kSurfaceRaised);
    painter.drawRect(track);

    const int mid = track.center().x();
    if (metric.self_value && metric.enemy_value) {
        const double total = static_cast<double>(*metric.self_value) +
                             static_cast<double>(*metric.enemy_value);
        if (total > 0.0) {
            const double share = static_cast<double>(*metric.self_value) / total;
            const int span = static_cast<int>(
                std::lround((share - 0.5) * 2.0 * (track.width() / 2.0)));
            painter.setBrush(stance_colour(stance));
            if (span >= 0)
                painter.drawRect(QRect(mid, track.top(), std::max(span, 1), track.height()));
            else
                painter.drawRect(
                    QRect(mid + span, track.top(), std::max(-span, 1), track.height()));
        }
    }
    painter.setBrush(theme::kTextMuted);
    painter.drawRect(QRect(mid - 1, track.top() - 1, 2, track.height() + 2));
}

}  // namespace

BattleAnalysisPane::BattleAnalysisPane(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(kContentHeight);
}

void BattleAnalysisPane::setMetrics(const std::vector<AnalysisMetric>& metrics) {
    metrics_ = metrics;
    update();
}

void BattleAnalysisPane::setFortressHold(FortressHold hold) {
    hold_ = hold;
    update();
}

QSize BattleAnalysisPane::sizeHint() const { return QSize(460, kContentHeight); }

void BattleAnalysisPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const QRect banner(0, 0, w, kBannerHeight);
    const QColor accent = hold_colour(hold_);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 38));
    painter.drawPath(theme::skewPath(QRectF(banner), 6));
    painter.setBrush(accent);
    painter.drawRect(QRect(0, banner.top() + 5, 3, banner.height() - 10));

    painter.setFont(theme::labelFont(9));
    painter.setPen(theme::kTextMuted);
    painter.drawText(QRect(banner.left() + 12, banner.top(), 40, kBannerHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("堡垒"));

    painter.setFont(theme::labelFont(15, true));
    painter.setPen(accent);
    painter.drawText(QRect(banner.left() + 50, banner.top(), w - 58, kBannerHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, fortress_hold_text(hold_));

    const int card_w = (w - kCardGap * (kMetricCount - 1)) / kMetricCount;
    const int top = kBannerHeight + kBannerGap;
    int index = 0;
    for (const AnalysisMetric& metric : metrics_) {
        if (index >= kMetricCount) break;
        const MetricStance stance = metric_stance(metric);
        const QRect card(index * (card_w + kCardGap), top, card_w, kCardHeight);

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::kSurfaceRaised);
        painter.drawPath(theme::skewPath(QRectF(card), 6));

        painter.setFont(theme::labelFont(9));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(card.left() + 10, card.top() + 4, card_w - 20, 13),
                         Qt::AlignLeft | Qt::AlignVCenter, metric.label);

        painter.setFont(theme::numericFont(11, true));
        painter.setPen(stance_colour(stance));
        painter.drawText(QRect(card.left() + 10, card.top() + 4, card_w - 20, 13),
                         Qt::AlignRight | Qt::AlignVCenter, diff_text(metric));

        painter.setFont(theme::numericFont(21, true));
        painter.setPen(metric.self_value ? theme::kTextPrimary : theme::kTextMuted);
        painter.drawText(QRect(card.left() + 10, card.top() + 18, card_w - 20, 25),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         metric_value_text(metric.self_value));

        painter.setFont(theme::numericFont(12, true));
        painter.setPen(metric.enemy_value ? theme::kTextSecondary : theme::kTextMuted);
        painter.drawText(QRect(card.left() + 10, card.top() + 21, card_w - 20, 22),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("敌 %1").arg(metric_value_text(metric.enemy_value)));

        paint_balance(painter, QRect(card.left() + 10, card.top() + 49, card_w - 20, 5),
                      metric, stance);
        ++index;
    }
}

}  // namespace rm_terminal
