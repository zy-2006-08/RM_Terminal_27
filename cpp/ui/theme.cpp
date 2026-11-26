#include "theme.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QRadialGradient>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <vector>

namespace rm_terminal::theme
{

    namespace
    {

        QFont makeFont(const QString& family, int point_size, bool bold, QFont::StyleHint hint)
        {
            ensureFonts();
            QFont font(family);
            font.setStyleHint(hint);
            font.setPointSize(point_size);
            font.setBold(bold);
            return font;
        }

        // 「F9」「REC」「1/3」这类拉丁字母和数字一起用数字字体,中文用黑体。
        bool is_latin(QChar c) { return c.unicode() < 128 && c.isLetterOrNumber(); }

        // 数字串可以带小数点、冒号、斜杠和百分号(「1.00」「03:56」「46/200」「100%」),
        // 但这些符号只在两边都是字母数字时才算进去。
        std::vector<std::pair<QString, bool>> mixed_runs(const QString& text)
        {
            std::vector<std::pair<QString, bool>> runs;
            int i = 0;
            while (i < text.size())
            {
                const bool latin = is_latin(text.at(i));
                int j = i + 1;
                while (j < text.size())
                {
                    const QChar c = text.at(j);
                    if (latin)
                    {
                        const bool joiner = (c == QLatin1Char('.') || c == QLatin1Char(':') || c == QLatin1Char('/')) &&
                                            j + 1 < text.size() && is_latin(text.at(j + 1));
                        if (!(is_latin(c) || joiner || c == QLatin1Char('%')))
                            break;
                    }
                    else if (is_latin(c))
                    {
                        break;
                    }
                    ++j;
                }
                runs.emplace_back(text.mid(i, j - i), latin);
                i = j;
            }
            return runs;
        }

        // 拉丁串紧挨着中文时两边各留一点空,紧挨空格或标点时不留。
        bool needs_gap(QChar c) { return c.unicode() >= 0x2E80; }

        constexpr qreal kNumberGap = 3.0;

    } // namespace

    QString css(const QColor& colour)
    {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(colour.red())
            .arg(colour.green())
            .arg(colour.blue())
            .arg(colour.alpha());
    }

    QString numericFamily() { return QStringLiteral("Barlow Semi Condensed"); }

    QString labelFamily()
    {
#if defined(Q_OS_MACOS)
        return QStringLiteral("PingFang SC");
#else
        return QStringLiteral("Noto Sans CJK SC");
#endif
    }

    void ensureFonts()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        for (const char* face : {"Regular", "Medium", "SemiBold", "Bold"})
            QFontDatabase::addApplicationFont(
                QStringLiteral(":/fonts/BarlowSemiCondensed-%1.ttf").arg(QLatin1String(face)));
    }

    QFont numericPx(int pixel_size, int weight)
    {
        ensureFonts();
        QFont font(numericFamily());
        font.setStyleHint(QFont::SansSerif);
        font.setPixelSize(std::max(1, pixel_size));
        font.setWeight(static_cast<QFont::Weight>(weight));
        font.setFeature(QFont::Tag("tnum"), 1);
        return font;
    }

    QFont labelPx(int pixel_size, int weight)
    {
        ensureFonts();
        QFont font(labelFamily());
        font.setStyleHint(QFont::SansSerif);
        font.setPixelSize(std::max(1, pixel_size));
        font.setWeight(static_cast<QFont::Weight>(weight));
        return font;
    }

    qreal paneScale(const QSizeF& actual, const QSizeF& design, qreal minimum, qreal maximum)
    {
        if (design.width() <= 0 || design.height() <= 0)
            return 1.0;
        const qreal by_width = actual.width() / design.width();
        const qreal by_height = actual.height() / design.height();
        return std::clamp(std::min(by_width, by_height), minimum, maximum);
    }

    int scaledPx(qreal design_px, qreal scale, int minimum)
    {
        return std::max(minimum, static_cast<int>(std::lround(design_px * scale)));
    }

    int fitFontPx(const QString& text, int wanted_px, int floor_px, qreal width,
                  const std::function<QFont(int)>& make_font)
    {
        int size = std::max(wanted_px, floor_px);
        while (size > floor_px && QFontMetricsF(make_font(size)).horizontalAdvance(text) > width)
            --size;
        return size;
    }

    QFont numericFont(int point_size, bool bold)
    {
        QFont font = makeFont(numericFamily(), point_size, bold, QFont::SansSerif);
        font.setFeature(QFont::Tag("tnum"), 1);
        return font;
    }

    QFont labelFont(int point_size, bool bold)
    {
        return makeFont(labelFamily(), point_size, bold, QFont::SansSerif);
    }

    QColor healthColor(double ratio)
    {
        if (ratio >= 0.5)
            return kOk;
        if (ratio > 0.25)
            return kWarn;
        return kDanger;
    }

    QColor eventColor(std::uint32_t level)
    {
        switch (level)
        {
        case 0: return kTextPrimary;
        case 1: return kOk;
        case 2:
        case 3: return kDanger;
        default: return kTextSecondary;
        }
    }

    QPainterPath chamferPath(const QRectF& box, qreal cut, bool mirrored)
    {
        const qreal c = std::clamp(cut, 0.0, std::min(box.width(), box.height()) / 2.0);
        const qreal l = box.left(), t = box.top(), r = box.right(), b = box.bottom();
        QPainterPath path;
        if (mirrored)
        {
            path.moveTo(l, t);
            path.lineTo(r - c, t);
            path.lineTo(r, t + c);
            path.lineTo(r, b);
            path.lineTo(l + c, b);
            path.lineTo(l, b - c);
        }
        else
        {
            path.moveTo(l + c, t);
            path.lineTo(r, t);
            path.lineTo(r, b - c);
            path.lineTo(r - c, b);
            path.lineTo(l, b);
            path.lineTo(l, t + c);
        }
        path.closeSubpath();
        return path;
    }

    void paintGlass(QPainter& painter, const QRectF& box, qreal cut, bool mirrored, const QColor& fill,
                    const QColor& stroke, bool highlight)
    {
        const QRectF inner = box.adjusted(0.5, 0.5, -0.5, -0.5);
        const QPainterPath shape = chamferPath(inner, cut, mirrored);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(stroke.alpha() > 0 ? QPen(stroke, 1.0) : QPen(Qt::NoPen));
        painter.setBrush(fill);
        painter.drawPath(shape);
        if (highlight)
        {
            painter.setPen(QPen(kHighlight, 1.0));
            const qreal y = box.top() + 1.5;
            if (mirrored)
                painter.drawLine(QPointF(box.left() + 2, y), QPointF(box.right() - cut - 2, y));
            else
                painter.drawLine(QPointF(box.left() + cut + 2, y), QPointF(box.right() - 2, y));
        }
        painter.restore();
    }

    QPainterPath skewBar(const QRectF& box, qreal degrees)
    {
        const qreal shift = std::tan(degrees * M_PI / 180.0) * box.height() / 2.0;
        QPainterPath path;
        path.moveTo(box.left() - shift, box.top());
        path.lineTo(box.right() - shift, box.top());
        path.lineTo(box.right() + shift, box.bottom());
        path.lineTo(box.left() + shift, box.bottom());
        path.closeSubpath();
        return path;
    }

    QPainterPath skewPath(const QRectF& box, qreal cut)
    {
        const qreal c = std::min(std::abs(cut), std::min(box.width(), box.height()) / 2.0);
        QPainterPath path;
        if (cut >= 0)
        {
            path.moveTo(box.left() + c, box.top());
            path.lineTo(box.right(), box.top());
            path.lineTo(box.right() - c, box.bottom());
            path.lineTo(box.left(), box.bottom());
        }
        else
        {
            path.moveTo(box.left(), box.top());
            path.lineTo(box.right() - c, box.top());
            path.lineTo(box.right(), box.bottom());
            path.lineTo(box.left() + c, box.bottom());
        }
        path.closeSubpath();
        return path;
    }

    QPainterPath skewPathLeading(const QRectF& box, qreal cut, bool mirrored)
    {
        const qreal c = std::min(std::abs(cut), std::min(box.width(), box.height()) / 2.0);
        QPainterPath path;
        if (mirrored)
        {
            path.moveTo(box.left(), box.top());
            path.lineTo(box.right(), box.top());
            path.lineTo(box.right(), box.bottom());
            path.lineTo(box.left() + c, box.bottom());
        }
        else
        {
            path.moveTo(box.left() + c, box.top());
            path.lineTo(box.right(), box.top());
            path.lineTo(box.right(), box.bottom());
            path.lineTo(box.left(), box.bottom());
        }
        path.closeSubpath();
        return path;
    }

    qreal mixedTextWidth(const QString& text, const QFont& label, const QFont& number)
    {
        const QFontMetricsF label_metrics(label);
        const QFontMetricsF number_metrics(number);
        qreal width = 0;
        const auto runs = mixed_runs(text);
        for (std::size_t i = 0; i < runs.size(); ++i)
        {
            const auto& [run, is_number] = runs[i];
            width += is_number ? number_metrics.horizontalAdvance(run) : label_metrics.horizontalAdvance(run);
            if (is_number && i > 0 && needs_gap(runs[i - 1].first.back()))
                width += kNumberGap;
            if (is_number && i + 1 < runs.size() && needs_gap(runs[i + 1].first.front()))
                width += kNumberGap;
        }
        return width;
    }

    qreal paintMixedText(QPainter& painter, const QRectF& box, Qt::Alignment alignment, const QString& text,
                         const QFont& label, const QFont& number, const QColor& colour)
    {
        const qreal total = mixedTextWidth(text, label, number);
        const QFontMetricsF label_metrics(label);
        const QFontMetricsF number_metrics(number);
        qreal x = box.left();
        if (alignment & Qt::AlignHCenter)
            x = box.center().x() - total / 2.0;
        else if (alignment & Qt::AlignRight)
            x = box.right() - total;
        // 两种字体按同一条基线排:取两者里较高的那个让整串在框里垂直居中。
        const qreal ascent = std::max(label_metrics.ascent(), number_metrics.ascent());
        const qreal descent = std::max(label_metrics.descent(), number_metrics.descent());
        const qreal baseline = box.center().y() + (ascent - descent) / 2.0;
        painter.save();
        painter.setPen(colour);
        const auto runs = mixed_runs(text);
        for (std::size_t i = 0; i < runs.size(); ++i)
        {
            const auto& [run, is_number] = runs[i];
            if (is_number && i > 0 && needs_gap(runs[i - 1].first.back()))
                x += kNumberGap;
            painter.setFont(is_number ? number : label);
            painter.drawText(QPointF(x, baseline), run);
            x += is_number ? number_metrics.horizontalAdvance(run) : label_metrics.horizontalAdvance(run);
            if (is_number && i + 1 < runs.size() && needs_gap(runs[i + 1].first.front()))
                x += kNumberGap;
        }
        painter.restore();
        return total;
    }

    void paintMetalText(QPainter& painter, const QRectF& box, const QString& text, int pixel_size,
                        Qt::Alignment alignment, bool italic)
    {
        if (text.isEmpty())
            return;
        const QFont font = numericPx(pixel_size, QFont::Bold);
        const QFontMetricsF metrics(font);
        QPainterPath path;
        path.addText(0, 0, font, text);
        if (italic)
            path = QTransform(1, 0, -0.2, 1, 0, 0).map(path);
        const QRectF ink = path.boundingRect();
        qreal x = box.left() - ink.left();
        if (alignment & Qt::AlignHCenter)
            x = box.left() + (box.width() - ink.width()) / 2.0 - ink.left();
        else if (alignment & Qt::AlignRight)
            x = box.right() - ink.width() - ink.left();
        const qreal baseline = box.center().y() + (metrics.ascent() - metrics.descent()) / 2.0;
        path.translate(x, baseline);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        // drop-shadow(0 2px 1.5px rgba(0,0,0,.55)):三层错开半像素叠出一点模糊。
        for (const qreal dy : {1.5, 2.0, 2.5})
            painter.fillPath(path.translated(0, dy), QColor(0, 0, 0, 62));
        QLinearGradient metal(0, box.top(), 0, box.bottom());
        metal.setColorAt(0.0, QColor(255, 255, 255));
        metal.setColorAt(0.35, QColor(255, 255, 255));
        metal.setColorAt(1.0, QColor(174, 185, 198));
        painter.fillPath(path, metal);
        painter.restore();
    }

    QImage backdrop(const QSize& size, qreal device_pixel_ratio)
    {
        struct Entry
        {
            QSize size;
            qreal dpr;
            QImage image;
        };
        static QMutex mutex;
        static std::vector<Entry> cache;
        const QMutexLocker lock(&mutex);
        if (size.isEmpty())
            return QImage();
        for (const Entry& entry : cache)
            if (entry.size == size && qFuzzyCompare(entry.dpr, device_pixel_ratio))
                return entry.image;

        const qreal dpr = std::max<qreal>(1.0, device_pixel_ratio);
        QImage image(static_cast<int>(std::ceil(size.width() * dpr)), static_cast<int>(std::ceil(size.height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        const qreal w = size.width();
        const qreal h = size.height();
        {
            QPainter painter(&image);
            QLinearGradient base(0, 0, 0, h);
            base.setColorAt(0, kVoid);
            base.setColorAt(1, kVoidDeep);
            painter.fillRect(QRectF(0, 0, w, h), base);
            // CSS radial-gradient(rx ry at cx cy, colour, transparent 70%):单位圆的渐变按椭圆
            // 半径拉伸后平移到圆心。透明端用同一个颜色、alpha 为 0,插值才不会发灰。
            const auto glow = [&](qreal cx, qreal cy, qreal rx, qreal ry, const QColor& colour, qreal clear_at)
            {
                QRadialGradient gradient(QPointF(0, 0), 1.0);
                QColor clear = colour;
                clear.setAlpha(0);
                gradient.setColorAt(0, colour);
                gradient.setColorAt(clear_at, clear);
                gradient.setColorAt(1, clear);
                QBrush brush(gradient);
                QTransform transform;
                transform.translate(cx, cy);
                transform.scale(rx, ry);
                brush.setTransform(transform);
                painter.fillRect(QRectF(0, 0, w, h), brush);
            };
            glow(w * 0.95, h * 0.75, w * 0.35, h * 0.35, QColor(22, 38, 64, 140), 0.7);
            glow(w * 0.08, h * 1.05, w * 0.45, h * 0.45, QColor(16, 44, 76, 191), 0.7);
            glow(w * 0.50, h * -0.08, w * 0.60, h * 0.55, QColor(34, 62, 104, 242), 0.7);
            // 暗角:以中心为圆心、经过四角的椭圆,55% 以外渐暗到 0.32。
            QRadialGradient vignette(QPointF(0, 0), 1.0);
            vignette.setColorAt(0, QColor(0, 0, 0, 0));
            vignette.setColorAt(0.55, QColor(0, 0, 0, 0));
            vignette.setColorAt(1, QColor(0, 0, 0, 82));
            QBrush shade(vignette);
            QTransform transform;
            transform.translate(w / 2, h / 2);
            transform.scale(w / 2 * M_SQRT2, h / 2 * M_SQRT2);
            shade.setTransform(transform);
            painter.fillRect(QRectF(0, 0, w, h), shade);
        }
        // 深色大面积渐变在 8 位里会出色带,加 ±1 的确定性抖动打散。
        for (int y = 0; y < image.height(); ++y)
        {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
            {
                std::uint32_t hash = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
                hash = (hash ^ (hash >> 13)) * 1274126177u;
                const int delta = static_cast<int>((hash >> 16) % 3) - 1;
                const QRgb p = line[x];
                line[x] = qRgb(std::clamp(qRed(p) + delta, 0, 255), std::clamp(qGreen(p) + delta, 0, 255),
                               std::clamp(qBlue(p) + delta, 0, 255));
            }
        }
        if (cache.size() >= 3)
            cache.erase(cache.begin());
        cache.push_back({size, device_pixel_ratio, image});
        return image;
    }

    QString styleSheet()
    {
        return QStringLiteral(R"(
QLabel {
    color: %1;
    background: transparent;
}
QLabel[heading="true"] {
    color: %2;
    font-weight: 600;
    letter-spacing: 2px;
    padding: 2px 0px 5px 0px;
    border-bottom: 1px solid %3;
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
    background: %3;
    border-radius: 4px;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover {
    background: %4;
}
QScrollBar:horizontal {
    background: transparent;
    height: 8px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: %3;
    border-radius: 4px;
    min-width: 24px;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    width: 0;
    background: transparent;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    height: 0;
    background: transparent;
}
QToolTip {
    color: %1;
    background: %5;
    border: 1px solid %4;
    padding: 4px 6px;
}
QDialog {
    background: %6;
}
QComboBox {
    background: %7;
    color: %1;
    border: 1px solid %3;
    padding: 4px 10px;
    min-height: 20px;
}
QComboBox:disabled {
    color: %8;
}
QComboBox::drop-down {
    border: none;
    width: 18px;
}
QComboBox QAbstractItemView {
    background: %6;
    color: %1;
    border: 1px solid %4;
    selection-background-color: %9;
    outline: none;
}
QCheckBox {
    color: %1;
    spacing: 8px;
}
QCheckBox:disabled {
    color: %8;
}
QTableWidget, QListWidget, QLineEdit, QSpinBox, QTimeEdit {
    background: %7;
    color: %1;
    border: 1px solid %3;
    gridline-color: %3;
    selection-background-color: %9;
}
QCheckBox::indicator, QListView::indicator {
    width: 16px;
    height: 16px;
    border: 1px solid %4;
    background: %7;
}
QCheckBox::indicator:checked, QListView::indicator:checked {
    background: %9;
    border-color: %10;
    image: url(:/images/ui/check.png);
}
QCheckBox::indicator:disabled, QListView::indicator:disabled {
    border-color: %3;
    background: transparent;
}
QPushButton {
    background: %7;
    color: %1;
    border: 1px solid %3;
    padding: 5px 14px;
}
QPushButton:hover {
    border-color: %10;
}
QPushButton:pressed, QPushButton:checked {
    background: %9;
}
QPushButton:disabled {
    color: %8;
}
QHeaderView::section {
    background: %6;
    color: %2;
    border: none;
    border-bottom: 1px solid %3;
    padding: 4px 6px;
}
)")
            .arg(css(kTextPrimary), css(kTextSecondary), css(kBorder), css(kBorderBright), css(kOverlay),
                 css(QColor(10, 16, 28)), css(kChip), css(kTextMuted), css(QColor(64, 170, 230, 107)))
            .arg(css(kCyan));
    }

} // namespace rm_terminal::theme
