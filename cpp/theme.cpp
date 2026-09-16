#include "theme.h"

#include <algorithm>
#include <cmath>

namespace rm_terminal::theme {

namespace {
// Menlo ships with macOS and is the same face the old panels asked for by name.
// styleHint keeps a Linux build on some monospaced face rather than falling back
// to a proportional one, which would break the column alignment.
QFont makeFont(const QString& family, int point_size, bool bold, QFont::StyleHint hint) {
    QFont font(family);
    font.setStyleHint(hint);
    font.setPointSize(point_size);
    font.setBold(bold);
    return font;
}
}  // namespace

QFont numericFont(int point_size, bool bold) {
    return makeFont(QStringLiteral("Menlo"), point_size, bold, QFont::Monospace);
}

QFont labelFont(int point_size, bool bold) {
    return makeFont(QStringLiteral("PingFang SC"), point_size, bold, QFont::SansSerif);
}

QColor healthColor(double ratio) {
    if (ratio > 0.6) return kOk;
    if (ratio > 0.25) return kWarn;
    return kDanger;
}

QColor eventColor(std::uint32_t level) {
    switch (level) {
    case 0: return kTextPrimary;
    case 1: return kOk;
    case 2:
    case 3: return kDanger;
    default: return kTextSecondary;
    }
}

QPainterPath skewPath(const QRectF& box, qreal cut) {
    const qreal c = std::min(std::abs(cut), std::min(box.width(), box.height()) / 2.0);
    QPainterPath path;
    if (cut >= 0) {
        path.moveTo(box.left() + c, box.top());
        path.lineTo(box.right(), box.top());
        path.lineTo(box.right() - c, box.bottom());
        path.lineTo(box.left(), box.bottom());
    } else {
        path.moveTo(box.left(), box.top());
        path.lineTo(box.right() - c, box.top());
        path.lineTo(box.right(), box.bottom());
        path.lineTo(box.left() + c, box.bottom());
    }
    path.closeSubpath();
    return path;
}

QPainterPath skewPathLeading(const QRectF& box, qreal cut, bool mirrored) {
    const qreal c = std::min(std::abs(cut), std::min(box.width(), box.height()) / 2.0);
    QPainterPath path;
    if (mirrored) {
        path.moveTo(box.left(), box.top());
        path.lineTo(box.right(), box.top());
        path.lineTo(box.right(), box.bottom());
        path.lineTo(box.left() + c, box.bottom());
    } else {
        path.moveTo(box.left() + c, box.top());
        path.lineTo(box.right(), box.top());
        path.lineTo(box.right(), box.bottom());
        path.lineTo(box.left(), box.bottom());
    }
    path.closeSubpath();
    return path;
}

QString styleSheet() {
    return QStringLiteral(R"(
QWidget#dashboardRoot {
    background: %1;
}
QFrame[card="true"] {
    background: %2;
    border: none;
    border-top: 2px solid %3;
    border-radius: 0px;
}
QLabel {
    color: %4;
    background: transparent;
}
/* 不可改回 kCyan:标题是静态标签,一屏五六个高饱和亮青同时发光会盖掉真正该抢眼的告警。
   参考端也只把青色给 LATEST / ALLY UNIT 这类小字副标题。 */
QLabel[heading="true"] {
    color: %7;
    font-weight: 600;
    letter-spacing: 2px;
    padding: 2px 0px 5px 0px;
    border-bottom: 1px solid %8;
}
QScrollArea {
    background: transparent;
    border: none;
}
QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: %5;
    border-radius: 4px;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover {
    background: %6;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    height: 0;
    background: transparent;
}
)")
        .arg(kVoid.name())
        .arg(kSurface.name())
        .arg(kBorderBright.name())
        .arg(kTextPrimary.name())
        .arg(kBorder.name())
        .arg(kBorderBright.name())
        .arg(kTextSecondary.name())
        .arg(kBorder.name());
}

}  // namespace rm_terminal::theme
