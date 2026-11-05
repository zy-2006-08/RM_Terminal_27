#pragma once

#include "layout_profile.h"
#include "theme.h"

#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QWidget>
#include <optional>
#include <vector>

class QVBoxLayout;

namespace rm_terminal {

// 玻璃面板:切角底 + 标题栏(斜竖条、标题、右侧小字)+ 标题下一道渐隐线。
// 卡片自己的布局就是内容区,标题栏高 kHeaderHeight,内容区四周默认留 8。
class GlassCard : public QFrame {
    Q_OBJECT
public:
    static constexpr int kHeaderHeight = 40;

    explicit GlassCard(const QString& title, QWidget* parent = nullptr);

    QString title() const { return title_; }
    void setTitle(const QString& title);
    void setAccent(const QColor& accent);
    // 右侧那一列的面板切右上和左下。
    void setMirrored(bool mirrored);
    bool mirrored() const { return mirrored_; }
    // 底色。默认是半透明的玻璃;浮在别的面板上面的(「下一步」急事弹出)要不透明。
    void setFill(const QColor& fill);
    // 标题栏右侧的小字,富文本。空串隐藏。
    void setHeaderRight(const QString& rich_text);
    QString headerRight() const;
    // 标题栏右边小字往左让出多少像素(界面设置页里给齿轮和叉留地方)。
    void setHeaderInset(int pixels);
    QVBoxLayout* body() const { return body_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void placeHeaderRight();

    QString title_;
    QColor accent_ = theme::kCyan;
    QColor fill_ = theme::kSurface;
    int header_inset_ = 0;
    bool mirrored_ = false;
    QLabel* header_right_;
    QVBoxLayout* body_;
};

// 切角按钮。Command 是面板里的指令按钮,Chip 是状态条上的小牌子,Accent 是要按住的
// 那种(能量机关、发射飞镖)。文字里的数字自动换成数字字体。
class GlassButton : public QPushButton {
    Q_OBJECT
public:
    enum class Kind { Command, Chip, Accent };

    explicit GlassButton(const QString& text, QWidget* parent = nullptr, Kind kind = Kind::Command);

    void setKind(Kind kind);
    Kind kind() const { return kind_; }
    // 按住进度 0~1,从左往右填满;0 不画。
    void setProgress(double progress);
    double progress() const { return progress_; }
    // 状态条小牌子:文字前的小圆点,透明色不画。
    void setDot(const QColor& colour);
    // 状态条小牌子:从第一个「 · 」(没有就从最后一个空格)起的尾巴用暗一档的字。
    void setDimTail(bool dim);
    // 「下一步」用:右侧的编号角标和一圈点缀色发光。
    void setBadge(const QString& badge);
    void setGlow(bool glow);
    // − / + 这种只放一个符号的方块按钮。
    void setSquare(bool square);
    void setTextColour(const QColor& colour);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QFont labelFont() const;
    QFont numberFont() const;

    Kind kind_;
    double progress_ = 0.0;
    QColor dot_ = Qt::transparent;
    QColor text_colour_ = Qt::transparent;
    bool dim_tail_ = false;
    QString badge_;
    bool glow_ = false;
    bool square_ = false;
    bool hovered_ = false;
};

// 状态条上只用来看的小牌子:席位、链路灯、录制。可以在文字前画一道阵营色斜竖条。
class ChipLabel : public QLabel {
    Q_OBJECT
public:
    enum class Tone { Normal, Alert };

    explicit ChipLabel(QWidget* parent = nullptr);
    void setTone(Tone tone);
    void setTick(const QColor& colour);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void applyMargins();

    Tone tone_ = Tone::Normal;
    QColor tick_ = Qt::transparent;
};

// 把一个控件按固定宽高比放进自己的格子里:能多宽就多宽,高度不够就按高度缩,水平和
// 垂直都居中。地图用它,场地永远按 28:15 画,格子多出来的地方留给卡片底色。
class AspectFrame : public QWidget {
public:
    AspectFrame(QWidget* child, double aspect, QWidget* parent = nullptr);
    // 紧贴在画面下方的一条(指挥手的标记工具条),和画面一起居中。
    void setFooter(QWidget* footer);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* child_;
    QWidget* footer_ = nullptr;
    double aspect_;
};

// 地图上方的提醒横幅:整条发光底、大字,底下一道细线表示还剩多久消失。
// 不接收鼠标,盖在地图上不挡点击。
class GlowBanner : public QWidget {
    Q_OBJECT
public:
    enum class Tone { Info, Warning, Critical };

    explicit GlowBanner(QWidget* parent = nullptr);
    void show(const QString& text, Tone tone, double remaining);
    QString text() const { return text_; }
    // 横幅的自然高度:66。
    static constexpr int kHeight = 66;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString text_;
    Tone tone_ = Tone::Info;
    double remaining_ = 1.0;
    QPixmap icon_;
};

// 一份布局的缩略图:每块面板一个小方块,highlight 那块用点缀色。界面设置页的方案按钮上用。
void paintLayoutThumbnail(QPainter& painter, const QRectF& box, const std::vector<PaneSlot>& cells,
                          std::optional<PaneId> highlight);

}  // namespace rm_terminal
