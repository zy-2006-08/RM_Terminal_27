#include "battle_analysis_pane.h"

#include "theme.h"

#include <QHash>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <algorithm>
#include <cmath>

namespace rm_terminal {

namespace {

constexpr int kBannerHeight = 34;
constexpr int kBannerGap = 6;
constexpr int kCardGap = 6;
constexpr int kCardHeight = 62;
constexpr int kMetricCount = 4;
constexpr int kIconSize = 11;
constexpr int kIconGap = 4;
// 最小高度按内容反推,和 self_status_pane 同样的理由:写死高度会让面板把窗口最小
// 高度顶到 720 以上,dashboard_layout 的 720p 断言就是守这个。改布局只改这里。
constexpr int kContentHeight = kBannerHeight + kBannerGap + kCardHeight;
// 全屏下这一格分到的高度远超 kContentHeight,按定高绘制会在卡片下方留一大片空白。
// 按实际高度等比放大,并封顶 1.8 倍 —— 再大字号就会盖过顶栏比分的视觉层级。
constexpr qreal kMaxScale = 1.8;

qreal content_scale(int available_height) {
    if (available_height <= kContentHeight) return 1.0;
    return std::min(static_cast<qreal>(available_height) / kContentHeight, kMaxScale);
}

int scaled(int value, qreal scale) { return static_cast<int>(std::lround(value * scale)); }

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

// 素材是白色前景 + 透明背景,直接画在深色卡片上会比相邻标签文字亮一档。用
// SourceIn 把前景整体染成标签色,alpha 通道保持不变,视觉重量才和文字一致。
// 结果按「路径 + 尺寸 + DPR」缓存:paintEvent 每帧都会走到这里,不缓存就等于
// 每帧重新解码 PNG 并做一次全像素合成。
QPixmap tinted_icon(const QString& path, const QColor& colour, qreal dpr, int size) {
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(path, colour.name(QColor::HexArgb))
                            .arg(size)
                            .arg(dpr);
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd()) return *hit;

    QPixmap source(path);
    if (source.isNull()) return {};

    const int px = static_cast<int>(std::lround(size * dpr));
    QPixmap fitted =
        source.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap out(fitted.size());
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawPixmap(0, 0, fitted);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), colour);
    p.end();

    cache.insert(key, out);
    return out;
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
    const qreal scale = content_scale(height());
    const int banner_h = scaled(kBannerHeight, scale);
    const int card_h = scaled(kCardHeight, scale);
    const QRect banner(0, 0, w, banner_h);
    const QColor accent = hold_colour(hold_);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 38));
    painter.drawPath(theme::skewPath(QRectF(banner), 6));
    painter.setBrush(accent);
    painter.drawRect(QRect(0, banner.top() + 5, 3, banner.height() - 10));

    painter.setFont(theme::labelFont(scaled(9, scale)));
    painter.setPen(theme::kTextMuted);
    painter.drawText(QRect(banner.left() + 12, banner.top(), scaled(40, scale), banner_h),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("堡垒"));

    painter.setFont(theme::labelFont(scaled(15, scale), true));
    painter.setPen(accent);
    painter.drawText(QRect(banner.left() + scaled(50, scale), banner.top(),
                           w - scaled(58, scale), banner_h),
                     Qt::AlignLeft | Qt::AlignVCenter, fortress_hold_text(hold_));

    const int card_w = (w - kCardGap * (kMetricCount - 1)) / kMetricCount;
    const int top = banner_h + scaled(kBannerGap, scale);
    int index = 0;
    for (const AnalysisMetric& metric : metrics_) {
        if (index >= kMetricCount) break;
        const MetricStance stance = metric_stance(metric);
        const QRect card(index * (card_w + kCardGap), top, card_w, card_h);
        const int pad = scaled(10, scale);
        const int label_h = scaled(13, scale);

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::kSurfaceRaised);
        painter.drawPath(theme::skewPath(QRectF(card), 6));

        painter.setFont(theme::labelFont(scaled(9, scale)));
        painter.setPen(theme::kTextMuted);
        int label_x = card.left() + pad;
        if (!metric.icon.isEmpty()) {
            const int icon_size = scaled(kIconSize, scale);
            const QPixmap icon = tinted_icon(metric.icon, theme::kTextMuted,
                                             devicePixelRatioF(), icon_size);
            if (!icon.isNull()) {
                const int icon_y = card.top() + scaled(4, scale) + (label_h - icon_size) / 2;
                painter.drawPixmap(label_x, icon_y, icon);
                label_x += icon_size + scaled(kIconGap, scale);
            }
        }
        painter.drawText(
            QRect(label_x, card.top() + scaled(4, scale), card.right() - pad - label_x, label_h),
            Qt::AlignLeft | Qt::AlignVCenter, metric.label);

        painter.setFont(theme::numericFont(scaled(11, scale), true));
        painter.setPen(stance_colour(stance));
        painter.drawText(
            QRect(card.left() + pad, card.top() + scaled(4, scale), card_w - pad * 2, label_h),
            Qt::AlignRight | Qt::AlignVCenter, diff_text(metric));

        painter.setFont(theme::numericFont(scaled(21, scale), true));
        painter.setPen(metric.self_value ? theme::kTextPrimary : theme::kTextMuted);
        painter.drawText(QRect(card.left() + pad, card.top() + scaled(18, scale),
                               card_w - pad * 2, scaled(25, scale)),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         metric_value_text(metric.self_value));

        painter.setFont(theme::numericFont(scaled(12, scale), true));
        painter.setPen(metric.enemy_value ? theme::kTextSecondary : theme::kTextMuted);
        painter.drawText(QRect(card.left() + pad, card.top() + scaled(21, scale),
                               card_w - pad * 2, scaled(22, scale)),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("敌 %1").arg(metric_value_text(metric.enemy_value)));

        paint_balance(painter, QRect(card.left() + pad, card.top() + scaled(49, scale),
                                     card_w - pad * 2, scaled(5, scale)),
                      metric, stance);
        ++index;
    }
}

}  // namespace rm_terminal
