#include "glass_items.h"

#include "theme.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QStringList>
#include <QTransform>
#include <QWidget>
#include <qqml.h>

#include <algorithm>
#include <cmath>

namespace rm_terminal
{

    namespace
    {

        void register_glass_types()
        {
            qmlRegisterType<GlassShape>("RmTerminal.Glass", 1, 0, "GlassShape");
            qmlRegisterType<MetalText>("RmTerminal.Glass", 1, 0, "MetalText");
            qmlRegisterType<SoftGlow>("RmTerminal.Glass", 1, 0, "SoftGlow");
            qmlRegisterType<GlassImage>("RmTerminal.Glass", 1, 0, "GlassImage");
        }

        // 斜体的切变量:x 向右偏 0.2×高度,约 11 度,和官方字样的倾角相近。
        constexpr qreal kItalicShear = 0.2;

        QString backdrop_id(const QWidget* host)
        {
            const QWidget* window = host->window();
            if (!window || window->size().isEmpty() || host->size().isEmpty())
                return QString();
            const QPoint at = host->mapTo(window, QPoint(0, 0));
            return QStringLiteral("image://backdrop/%1/%2/%3/%4/%5/%6/%7")
                .arg(window->width())
                .arg(window->height())
                .arg(at.x())
                .arg(at.y())
                .arg(host->width())
                .arg(host->height())
                .arg(qRound(host->devicePixelRatioF() * 100));
        }

        // 宿主自己挪动、改大小,或者窗口改大小时重算背景切片的地址。
        class BackdropTracker final : public QObject
        {
        public:
            explicit BackdropTracker(QQuickWidget* host) : QObject(host), host_(host)
            {
                host->installEventFilter(this);
                watchWindow();
            }

        protected:
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                switch (event->type())
                {
                case QEvent::Move:
                case QEvent::Resize:
                case QEvent::Show:
                    if (watched == host_)
                        watchWindow();
                    refresh();
                    break;
                case QEvent::ParentChange:
                    watchWindow();
                    refresh();
                    break;
                default:
                    break;
                }
                return false;
            }

        private:
            void watchWindow()
            {
                QWidget* window = host_->window();
                if (window == window_)
                    return;
                if (window_)
                    window_->removeEventFilter(this);
                window_ = window;
                if (window_ && window_ != host_)
                    window_->installEventFilter(this);
            }

            void refresh()
            {
                QQuickItem* root = host_->rootObject();
                if (!root || root->metaObject()->indexOfProperty("backdropSource") < 0)
                    return;
                const QString id = backdrop_id(host_);
                if (root->property("backdropSource").toString() != id)
                    root->setProperty("backdropSource", id);
            }

            QQuickWidget* host_;
            QPointer<QWidget> window_;
        };

    } // namespace

    Q_COREAPP_STARTUP_FUNCTION(register_glass_types)

    GlassShape::GlassShape(QQuickItem* parent) : QQuickPaintedItem(parent)
    {
        setAntialiasing(true);
        connect(this, &GlassShape::changed, this, [this]
                { update(); });
    }

    void GlassShape::paint(QPainter* painter)
    {
        const QRectF box(0, 0, width(), height());
        if (!has_fill_end_)
        {
            theme::paintGlass(*painter, box, cut_, mirrored_, fill_, stroke_, highlight_);
            return;
        }
        QLinearGradient gradient(0, 0, width(), 0);
        gradient.setColorAt(0, fill_);
        gradient.setColorAt(std::clamp(fill_end_at_, 0.01, 1.0), fill_end_);
        gradient.setColorAt(1, fill_end_);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(gradient);
        painter->drawPath(theme::chamferPath(box, cut_, mirrored_));
    }

    void GlassShape::setFillEnd(const QColor& colour)
    {
        if (has_fill_end_ && fill_end_ == colour)
            return;
        fill_end_ = colour;
        has_fill_end_ = true;
        Q_EMIT changed();
    }

    void GlassShape::setFillEndAt(qreal at)
    {
        if (qFuzzyCompare(fill_end_at_, at))
            return;
        fill_end_at_ = at;
        Q_EMIT changed();
    }

    void GlassShape::setCut(qreal cut)
    {
        if (qFuzzyCompare(cut_, cut))
            return;
        cut_ = cut;
        Q_EMIT changed();
    }

    void GlassShape::setMirrored(bool mirrored)
    {
        if (mirrored_ == mirrored)
            return;
        mirrored_ = mirrored;
        Q_EMIT changed();
    }

    void GlassShape::setFillColor(const QColor& colour)
    {
        if (fill_ == colour)
            return;
        fill_ = colour;
        Q_EMIT changed();
    }

    void GlassShape::setStrokeColor(const QColor& colour)
    {
        if (stroke_ == colour)
            return;
        stroke_ = colour;
        Q_EMIT changed();
    }

    void GlassShape::setHighlight(bool on)
    {
        if (highlight_ == on)
            return;
        highlight_ = on;
        Q_EMIT changed();
    }

    SoftGlow::SoftGlow(QQuickItem* parent) : QQuickPaintedItem(parent)
    {
        setAntialiasing(true);
        connect(this, &SoftGlow::changed, this, [this]
                { update(); });
    }

    void SoftGlow::paint(QPainter* painter)
    {
        if (color_.alpha() == 0 || width() <= 0 || height() <= 0)
            return;
        QRadialGradient gradient(QPointF(0, 0), 1.0);
        QColor clear = color_;
        clear.setAlpha(0);
        gradient.setColorAt(0, color_);
        gradient.setColorAt(1, clear);
        QBrush brush(gradient);
        QTransform transform;
        transform.translate(width() / 2, height() / 2);
        transform.scale(width() / 2, height() / 2);
        brush.setTransform(transform);
        painter->fillRect(QRectF(0, 0, width(), height()), brush);
    }

    void SoftGlow::setColor(const QColor& colour)
    {
        if (color_ == colour)
            return;
        color_ = colour;
        Q_EMIT changed();
    }

    GlassImage::GlassImage(QQuickItem* parent) : QQuickPaintedItem(parent)
    {
        setAntialiasing(true);
        connect(this, &GlassImage::changed, this, [this]
                { update(); });
    }

    void GlassImage::paint(QPainter* painter)
    {
        if (source_.isEmpty() || width() <= 0 || height() <= 0)
            return;
        static QHash<QString, QImage> cache;
        const QString key = source_ + (gray_ ? QStringLiteral("|gray") : QString());
        auto it = cache.find(key);
        if (it == cache.end())
        {
            QString path = source_;
            if (path.startsWith(QStringLiteral("qrc:")))
                path = path.mid(3);
            QImage image(path);
            if (!image.isNull() && gray_)
            {
                image = image.convertToFormat(QImage::Format_ARGB32);
                for (int y = 0; y < image.height(); ++y)
                {
                    auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
                    for (int x = 0; x < image.width(); ++x)
                    {
                        const int v = qGray(line[x]) * 3 / 4;
                        line[x] = qRgba(v, v, v, qAlpha(line[x]));
                    }
                }
            }
            it = cache.insert(key, image);
        }
        if (it->isNull())
            return;
        const QSizeF fitted = QSizeF(it->size()).scaled(width(), height(), Qt::KeepAspectRatio);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->drawImage(QRectF((width() - fitted.width()) / 2, (height() - fitted.height()) / 2, fitted.width(),
                                  fitted.height()),
                           *it);
    }

    void GlassImage::setSource(const QString& source)
    {
        if (source_ == source)
            return;
        source_ = source;
        Q_EMIT changed();
    }

    void GlassImage::setGray(bool gray)
    {
        if (gray_ == gray)
            return;
        gray_ = gray;
        Q_EMIT changed();
    }

    MetalText::MetalText(QQuickItem* parent) : QQuickPaintedItem(parent)
    {
        setAntialiasing(true);
        connect(this, &MetalText::changed, this, [this]
                {
        updateImplicitSize();
        update(); });
        updateImplicitSize();
    }

    void MetalText::updateImplicitSize()
    {
        const QFontMetricsF metrics(theme::numericPx(pixel_size_, QFont::Bold));
        const qreal slant = italic_ ? metrics.ascent() * kItalicShear : 0;
        setImplicitSize(metrics.horizontalAdvance(text_) + slant + 2, metrics.height());
    }

    void MetalText::paint(QPainter* painter)
    {
        theme::paintMetalText(*painter, QRectF(0, 0, width(), height()), text_, pixel_size_,
                              static_cast<Qt::Alignment>(alignment_) | Qt::AlignVCenter, italic_);
    }

    void MetalText::setText(const QString& text)
    {
        if (text_ == text)
            return;
        text_ = text;
        Q_EMIT changed();
    }

    void MetalText::setPixelSize(int size)
    {
        if (pixel_size_ == size)
            return;
        pixel_size_ = size;
        Q_EMIT changed();
    }

    void MetalText::setItalic(bool italic)
    {
        if (italic_ == italic)
            return;
        italic_ = italic;
        Q_EMIT changed();
    }

    void MetalText::setHorizontalAlignment(int alignment)
    {
        if (alignment_ == alignment)
            return;
        alignment_ = alignment;
        Q_EMIT changed();
    }

    BackdropProvider::BackdropProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage BackdropProvider::requestImage(const QString& id, QSize* size, const QSize&)
    {
        const QStringList parts = id.split(QLatin1Char('/'));
        if (parts.size() != 7)
            return QImage();
        int values[7];
        for (int i = 0; i < 7; ++i)
        {
            bool ok = false;
            values[i] = parts[i].toInt(&ok);
            if (!ok)
                return QImage();
        }
        const qreal dpr = std::max(1, values[6]) / 100.0;
        const QImage whole = theme::backdrop(QSize(values[0], values[1]), dpr);
        if (whole.isNull())
            return QImage();
        const QRect slice(qRound(values[2] * dpr), qRound(values[3] * dpr), qRound(values[4] * dpr),
                          qRound(values[5] * dpr));
        QImage out = whole.copy(slice);
        out.setDevicePixelRatio(dpr);
        if (size)
            *size = out.size();
        return out;
    }

    void prepare_quick_widget(QQuickWidget* host)
    {
        theme::ensureFonts();
        QQmlEngine* engine = host->engine();
        if (engine && !engine->imageProvider(QStringLiteral("backdrop")))
            engine->addImageProvider(QStringLiteral("backdrop"), new BackdropProvider);
        new BackdropTracker(host);
    }

} // namespace rm_terminal
