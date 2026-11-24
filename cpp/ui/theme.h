#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <cstdint>
#include <functional>

class QPainter;

namespace rm_terminal::theme {

// One place for every colour and font in the UI, mirrored value for value in
// qml/Theme.qml.
//
// 「深空玻璃」:数值取自用户认可的效果图 v5(~/图片/rm-ui-v5/1-深空玻璃.png 的 CSS)。
// 面板、行、按钮都是半透明的,底下透出窗口背景的三团光晕,所以这些颜色大多带 alpha。
// 写进样式表时要用 css(),QColor::name() 会丢掉 alpha。

// Window backdrop: vertical gradient top -> bottom, glows drawn by backdrop().
inline const QColor kVoid(7, 11, 20);
inline const QColor kVoidDeep(4, 6, 11);

// Glass layers, translucent on purpose.
inline const QColor kSurface(16, 26, 44, 153);       // panel
inline const QColor kSurfaceRaised(40, 58, 90, 97);  // button
inline const QColor kChip(12, 20, 34, 168);          // status chip
inline const QColor kRow(30, 44, 70, 102);           // roster row
inline const QColor kTile(255, 255, 255, 9);         // tile inside a panel
inline const QColor kBorder(150, 180, 230, 41);
inline const QColor kRowBorder(150, 180, 230, 26);
inline const QColor kBorderBright(160, 196, 255, 82);
inline const QColor kHighlight(255, 255, 255, 26);   // top edge of a panel
// 设置页、自检页、弹窗盖在面板上,不能透出底下的字。
inline const QColor kOverlay(10, 17, 30, 255);
inline const QColor kMapBackground(4, 8, 14, 140);
inline const QColor kVideoBackground(7, 11, 18);

// Faction identity. Deliberately not the same red as the alert palette: "my armour
// is red" and "this value is dangerous" must never look alike.
inline const QColor kRed(228, 62, 74);
inline const QColor kRedDim(96, 30, 38);
inline const QColor kBlue(46, 134, 222);
inline const QColor kBlueDim(24, 62, 104);
inline const QColor kRedText(255, 143, 150);
inline const QColor kBlueText(132, 194, 255);
// 血条、控制度条的发光填充色。
inline const QColor kFillRed(240, 82, 96);
inline const QColor kFillBlue(64, 152, 238);
inline const QColor kFillWarn(236, 170, 70);
inline const QColor kFillDanger(255, 88, 72);

// Accent: active borders, the newest event, the selected thing.
inline const QColor kCyan(142, 210, 242);
inline const QColor kCyanDim(84, 156, 204, 82);
inline const QColor kAccentText(196, 233, 251);
inline const QColor kAccentBg(18, 44, 66, 219);

// Text, brightest to dimmest.
inline const QColor kTextPrimary(230, 237, 246);
inline const QColor kTextBright(195, 206, 221);
inline const QColor kTextSecondary(147, 163, 186);
inline const QColor kTextMuted(111, 127, 152);

// Status semantics.
inline const QColor kOk(62, 207, 142);
inline const QColor kOkText(95, 211, 154);
inline const QColor kWarn(236, 170, 70);
inline const QColor kDanger(255, 100, 100);
inline const QColor kDangerText(255, 154, 154);
inline const QColor kOffline(91, 100, 116);

// rgba() for style sheets. QColor::name() drops the alpha channel.
QString css(const QColor& colour);

// 数字用随程序打包的 Barlow Semi Condensed(OFL),开 tnum 等宽数字,跳动的数值不会让
// 整行左右晃。中文用系统黑体:macOS 是苹方,其他平台是思源黑体(Noto Sans CJK SC)。
QString numericFamily();
QString labelFamily();
// 注册打包的字体。重复调用无副作用;字体资源没链进来时什么都不做,回落到系统字体。
void ensureFonts();
// 按像素指定字号,效果图里的尺寸都是像素。weight 用 QFont::Weight 的数值(500、600…)。
QFont numericPx(int pixel_size, int weight = QFont::DemiBold);
QFont labelPx(int pixel_size, int weight = QFont::Medium);

// 字号的下限。缩到这里还放不下就该少画一行,而不是继续缩成看不清的字。
inline constexpr int kMinLabelPx = 11;
inline constexpr int kMinNumberPx = 12;
// 放大的上限。再大字就压过面板标题,面板看起来不像同一套界面了。
inline constexpr qreal kMaxPaneScale = 2.5;

// 一块面板只有一个缩放系数:实际尺寸 ÷ 设计尺寸,宽高各算一遍取小的那个 —— 放大时
// 窄的那一边才不会溢出。操作手把面板拖大就放大、拖小就缩小,字号、块高、间距都乘它。
qreal paneScale(const QSizeF& actual, const QSizeF& design, qreal minimum = 0.7,
                qreal maximum = kMaxPaneScale);
// 设计稿上的像素值乘上系数,四舍五入,至少 minimum 像素。
int scaledPx(qreal design_px, qreal scale, int minimum = 1);

// 字号从 wanted 往下缩到这段文字画得进 width 为止,最低 floor_px。先缩字、再由调用方
// 决定省略:省略号是最后一招,一句话被切掉一半比小一号的字难读得多。
int fitFontPx(const QString& text, int wanted_px, int floor_px, qreal width,
              const std::function<QFont(int)>& make_font);
// 旧接口:按点数指定。保留给还没改到的面板。
QFont numericFont(int point_size, bool bold = false);
QFont labelFont(int point_size, bool bold = false);

// 官方选手端的飘字配色:系统中立信息白、有利信息绿、不利信息红。
// level: 0 信息 1 提示 2 警告 3 严重。
QColor eventColor(std::uint32_t level);

// 45 度切角,整套界面唯一的形状语言:面板切 10、卡片和行切 6、按钮和标签切 4。
// 不镜像时切左上和右下;镜像(右侧那一列)切右上和左下。
QPainterPath chamferPath(const QRectF& box, qreal cut, bool mirrored = false);
// 一块玻璃:切角底、1 像素描边,可选顶边一道高光。描边落在半像素上,线才是实的。
void paintGlass(QPainter& painter, const QRectF& box, qreal cut, bool mirrored, const QColor& fill,
                const QColor& stroke, bool highlight = false);
// 斜着的条(血条、控制度条、标题前的小竖条)。degrees 是 CSS skewX 的角度,绕中心斜切。
QPainterPath skewBar(const QRectF& box, qreal degrees);

// 斜切平行四边形。cut 为斜切的水平投影像素数,正值切左上/右下,负值反向。
QPainterPath skewPath(const QRectF& box, qreal cut);
// 单侧斜切:只切一端,用于紧贴屏幕边缘的面板,避免两头都缺角。
QPainterPath skewPathLeading(const QRectF& box, qreal cut, bool mirrored);

// 文字里的数字换成数字字体画:「兑换 50发 · 50金」里 50 用 Barlow,其余用黑体,
// 数字两边各留 3 像素,字形不同的两种字挨着时不挤。返回画出的宽度。
qreal paintMixedText(QPainter& painter, const QRectF& box, Qt::Alignment alignment, const QString& text,
                     const QFont& label, const QFont& number, const QColor& colour);
qreal mixedTextWidth(const QString& text, const QFont& label, const QFont& number);

// 官方「前哨站」字样那种斜体金属字:上白下灰的竖向渐变(按 box 的高度铺),下面一道投影。
// Barlow 没有斜体字形,斜体是切出来的。基地血量和「下一步」的排名数字用它。
void paintMetalText(QPainter& painter, const QRectF& box, const QString& text, int pixel_size,
                    Qt::Alignment alignment = Qt::AlignCenter, bool italic = true);

// 窗口背景:竖向渐变 + 三团径向光(上方正中最亮)+ 暗角,加一点抖动避免色带。
// 按尺寸缓存。Dashboard 画整张,QML 面板画自己那一块(见 glass_items.h)。
QImage backdrop(const QSize& size, qreal device_pixel_ratio);

// Health drives hue with the roster's thresholds: green from half up, amber above a
// quarter, red below.
QColor healthColor(double ratio);

// Whole-application stylesheet: labels, scrollbars, tooltips.
QString styleSheet();

}  // namespace rm_terminal::theme
