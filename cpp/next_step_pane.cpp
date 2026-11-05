#include "next_step_pane.h"

#include "theme.h"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <algorithm>

namespace rm_terminal {

NextStepPane::NextStepPane(QWidget* parent) : QWidget(parent) {}

void NextStepPane::setCards(const std::vector<NextCard>& cards) {
    const bool same = cards.size() == cards_.size() &&
                      std::equal(cards.begin(), cards.end(), cards_.begin(), [](const NextCard& a, const NextCard& b) {
                          return a.rule == b.rule && a.title == b.title && a.reason == b.reason;
                      });
    if (same) return;
    cards_ = cards;
    update();
}

void NextStepPane::setCardSize(int size) {
    if (size_ == size) return;
    size_ = size;
    update();
}

void NextStepPane::setCapacity(int capacity) {
    if (capacity_ == capacity) return;
    capacity_ = std::clamp(capacity, 1, 3);
    update();
}

QSize NextStepPane::sizeHint() const { return QSize(600, 92); }

QSize NextStepPane::minimumSizeHint() const { return QSize(240, 48); }

void NextStepPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (cards_.empty()) {
        painter.setFont(theme::labelPx(14));
        painter.setPen(theme::kTextSecondary);
        painter.drawText(rect().adjusted(12, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("现在没有要紧的事：盯住地图和血量，按自己的节奏打"));
        return;
    }
    const int columns = std::max<int>(capacity_, static_cast<int>(cards_.size()));
    const qreal gap = 8;
    const qreal width = (this->width() - gap * (columns - 1)) / columns;
    // 字号随卡片大小和格子高度一起变:格子矮时先缩原因那一行,再缩标题。
    const qreal k = std::clamp(height() / 92.0, 0.7, 1.3);
    const int title_px = static_cast<int>(std::lround((size_ == 0 ? 15 : size_ == 2 ? 21 : 18) * std::min(1.0, k)));
    const int reason_px = std::max(11, static_cast<int>(std::lround((size_ == 2 ? 14 : 12.5) * std::min(1.0, k))));
    const int rank_px = size_ == 0 ? 20 : 24;
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const NextCard& card = cards_[i];
        const bool top = i == 0;
        const QRectF box(i * (width + gap), 0, width, height());
        painter.save();
        painter.setClipPath(theme::chamferPath(box, 6));
        if (top) {
            QLinearGradient glow(box.topLeft(), box.bottomRight());
            glow.setColorAt(0, QColor(64, 170, 230, 77));
            glow.setColorAt(0.7, QColor(64, 170, 230, 20));
            glow.setColorAt(1, QColor(64, 170, 230, 20));
            painter.fillRect(box, glow);
        } else {
            painter.fillRect(box, QColor(255, 255, 255, 10));
        }
        painter.restore();
        // 左侧竖条:第 1 张是点缀色并发光。
        const QRectF bar(box.left(), box.top() + 10, 3, box.height() - 20);
        if (top) {
            painter.fillRect(bar.adjusted(-2, -2, 2, 2), QColor(142, 210, 242, 50));
            painter.fillRect(bar, theme::kCyan);
        } else {
            painter.fillRect(bar, theme::kTextMuted);
        }

        const qreal left = box.left() + 16;
        const qreal right = box.right() - 12;
        const qreal head_top = box.top() + std::max(6.0, 10 * k);
        theme::paintMetalText(painter, QRectF(left, head_top, 18, 22), QString::number(i + 1), rank_px,
                              Qt::AlignLeft | Qt::AlignVCenter);
        QFont tag_font = theme::labelPx(12, QFont::Bold);
        tag_font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        const qreal tag_w = QFontMetricsF(tag_font).horizontalAdvance(card.category) + 12;
        const QRectF tag(left + 24, head_top + 2, tag_w, 19);
        theme::paintGlass(painter, tag, 4, false, top ? QColor(18, 44, 66, 230) : QColor(255, 255, 255, 20),
                          Qt::transparent);
        painter.setFont(tag_font);
        painter.setPen(top ? theme::kAccentText : theme::kTextBright);
        painter.drawText(tag, Qt::AlignCenter, card.category);

        QFont title_font = theme::labelPx(title_px, QFont::Bold);
        title_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        const qreal title_top = head_top + 22 + std::max(3.0, 8 * k);
        const qreal title_h = title_px * 1.25;
        painter.setFont(title_font);
        painter.setPen(Qt::white);
        const QFontMetricsF title_metrics(title_font);
        painter.drawText(QRectF(left, title_top, right - left, title_h), Qt::AlignLeft | Qt::AlignVCenter,
                         title_metrics.elidedText(card.title, Qt::ElideRight, right - left));

        const qreal reason_top = title_top + title_h + std::max(2.0, 5 * k);
        if (reason_top + reason_px <= box.bottom() - 2) {
            const QFont reason_font = theme::labelPx(reason_px);
            painter.setFont(reason_font);
            painter.setPen(top ? theme::kTextBright : theme::kTextSecondary);
            painter.drawText(QRectF(left, reason_top, right - left, reason_px * 1.4), Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetricsF(reason_font).elidedText(card.reason, Qt::ElideRight, right - left));
        }
    }
    // 事情不够几件时,空着的位置画一张淡的占位卡,不留一块黑。
    for (int i = static_cast<int>(cards_.size()); i < columns; ++i) {
        const QRectF box(i * (width + gap), 0, width, height());
        painter.save();
        painter.setClipPath(theme::chamferPath(box, 6));
        painter.fillRect(box, QColor(255, 255, 255, 6));
        painter.restore();
        QPen dashed(QColor(142, 210, 242, 46), 1, Qt::DashLine);
        dashed.setDashPattern({4, 3});
        painter.setPen(dashed);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(theme::chamferPath(box.adjusted(0.5, 0.5, -0.5, -0.5), 6));
        painter.setFont(theme::labelPx(13));
        painter.setPen(theme::kTextMuted);
        painter.drawText(box, Qt::AlignCenter, QStringLiteral("暂时没有更要紧的事"));
    }
}

}  // namespace rm_terminal
