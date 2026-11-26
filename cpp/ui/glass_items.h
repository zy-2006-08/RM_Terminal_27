#pragma once

#include <QColor>
#include <QObject>
#include <QQuickImageProvider>
#include <QQuickPaintedItem>
#include <QString>

class QQmlEngine;
class QQuickWidget;

namespace rm_terminal
{

    // QML 里的玻璃切角块,和 C++ 面板共用 theme::paintGlass,两边画出来是同一种东西。
    class GlassShape : public QQuickPaintedItem
    {
        Q_OBJECT
        Q_PROPERTY(qreal cut READ cut WRITE setCut NOTIFY changed)
        Q_PROPERTY(bool mirrored READ mirrored WRITE setMirrored NOTIFY changed)
        Q_PROPERTY(QColor fillColor READ fillColor WRITE setFillColor NOTIFY changed)
        Q_PROPERTY(QColor strokeColor READ strokeColor WRITE setStrokeColor NOTIFY changed)
        Q_PROPERTY(bool highlight READ highlight WRITE setHighlight NOTIFY changed)
        // 设了 fillEnd 就画横向渐变:左端 fillColor,到宽度的 fillEndAt 处变成 fillEnd。
        Q_PROPERTY(QColor fillEnd READ fillEnd WRITE setFillEnd NOTIFY changed)
        Q_PROPERTY(qreal fillEndAt READ fillEndAt WRITE setFillEndAt NOTIFY changed)
    public:
        explicit GlassShape(QQuickItem* parent = nullptr);
        void paint(QPainter* painter) override;

        qreal cut() const { return cut_; }
        void setCut(qreal cut);
        bool mirrored() const { return mirrored_; }
        void setMirrored(bool mirrored);
        QColor fillColor() const { return fill_; }
        void setFillColor(const QColor& colour);
        QColor strokeColor() const { return stroke_; }
        void setStrokeColor(const QColor& colour);
        bool highlight() const { return highlight_; }
        void setHighlight(bool on);
        QColor fillEnd() const { return fill_end_; }
        void setFillEnd(const QColor& colour);
        qreal fillEndAt() const { return fill_end_at_; }
        void setFillEndAt(qreal at);

    Q_SIGNALS:
        void changed();

    private:
        QColor fill_end_ = Qt::transparent;
        qreal fill_end_at_ = 1.0;
        bool has_fill_end_ = false;
        qreal cut_ = 6;
        bool mirrored_ = false;
        QColor fill_ = Qt::transparent;
        QColor stroke_ = Qt::transparent;
        bool highlight_ = false;
    };

    // 一团柔光:椭圆,中心是 color,向边缘淡到全透明(CSS radial-gradient closest-side)。
    // 头像底下的阵营色光晕、地图上残血敌人的红光用它。
    class SoftGlow : public QQuickPaintedItem
    {
        Q_OBJECT
        Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY changed)
    public:
        explicit SoftGlow(QQuickItem* parent = nullptr);
        void paint(QPainter* painter) override;
        QColor color() const { return color_; }
        void setColor(const QColor& colour);

    Q_SIGNALS:
        void changed();

    private:
        QColor color_ = Qt::transparent;
    };

    // 按比例居中画一张 qrc 图片;gray 为真时画成压暗的灰度(阵亡的头像)。
    // 用 QPainter 而不是着色器效果:离屏截图和没有显卡的机器上着色器效果画不出来。
    class GlassImage : public QQuickPaintedItem
    {
        Q_OBJECT
        Q_PROPERTY(QString source READ source WRITE setSource NOTIFY changed)
        Q_PROPERTY(bool gray READ gray WRITE setGray NOTIFY changed)
    public:
        explicit GlassImage(QQuickItem* parent = nullptr);
        void paint(QPainter* painter) override;
        QString source() const { return source_; }
        void setSource(const QString& source);
        bool gray() const { return gray_; }
        void setGray(bool gray);

    Q_SIGNALS:
        void changed();

    private:
        QString source_;
        bool gray_ = false;
    };

    // 官方「前哨站」字样那种斜体金属字:上白下灰的竖向渐变,下面一道投影。基地血量、
    // 「下一步」的排名数字用它。斜体是自己切出来的,Barlow 没有斜体字形。
    class MetalText : public QQuickPaintedItem
    {
        Q_OBJECT
        Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
        Q_PROPERTY(int pixelSize READ pixelSize WRITE setPixelSize NOTIFY changed)
        Q_PROPERTY(bool italic READ italic WRITE setItalic NOTIFY changed)
        Q_PROPERTY(int horizontalAlignment READ horizontalAlignment WRITE setHorizontalAlignment NOTIFY changed)
    public:
        explicit MetalText(QQuickItem* parent = nullptr);
        void paint(QPainter* painter) override;

        QString text() const { return text_; }
        void setText(const QString& text);
        int pixelSize() const { return pixel_size_; }
        void setPixelSize(int size);
        bool italic() const { return italic_; }
        void setItalic(bool italic);
        int horizontalAlignment() const { return alignment_; }
        void setHorizontalAlignment(int alignment);

    Q_SIGNALS:
        void changed();

    private:
        void updateImplicitSize();

        QString text_;
        int pixel_size_ = 22;
        bool italic_ = true;
        int alignment_ = Qt::AlignHCenter;
    };

    // 窗口背景切片。QML 面板装在 QQuickWidget 里,看不到 Dashboard 画在下面的背景,
    // 所以各自画同一张背景里自己那一块:id 是「窗口宽/高/x/y/宽/高/dpr×100」。
    class BackdropProvider : public QQuickImageProvider
    {
    public:
        BackdropProvider();
        QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;
    };

    // QQuickWidget 加载 QML 之前调用:注册字体、装上背景切片,并让根对象的 backdropSource
    // 跟着控件在窗口里的位置和窗口大小走。根对象没有这个属性时只是不画背景。
    void prepare_quick_widget(QQuickWidget* host);

} // namespace rm_terminal
