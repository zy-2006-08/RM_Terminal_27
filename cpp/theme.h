#pragma once

#include <QColor>
#include <QFont>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <cstdint>

namespace rm_terminal::theme {

// One place for every colour and font in the UI.
//
// Previously each panel inlined its own setStyleSheet string, so "the heading
// blue" existed in six copies and drifted. Anything visual that appears twice
// belongs here.

// Surfaces, darkest to lightest.
inline const QColor kVoid(7, 9, 13);          // window behind everything
inline const QColor kSurface(14, 18, 26);     // card body
inline const QColor kSurfaceRaised(20, 26, 37);
inline const QColor kBorder(38, 48, 66);
inline const QColor kBorderBright(58, 78, 108);

// Faction identity. Deliberately not the same red/blue as the alert palette:
// "my armour is red" and "this value is dangerous" must never look alike.
inline const QColor kRed(228, 62, 74);
inline const QColor kRedDim(96, 30, 38);
inline const QColor kBlue(46, 134, 222);
inline const QColor kBlueDim(24, 62, 104);

// Accent used for headings, active borders, and the scanline.
inline const QColor kCyan(64, 208, 232);
inline const QColor kCyanDim(22, 78, 92);

// Text, brightest to dimmest.
inline const QColor kTextPrimary(226, 232, 240);
inline const QColor kTextSecondary(148, 163, 184);
inline const QColor kTextMuted(88, 100, 118);

// Status semantics. Deliberately desaturated: on a dark HUD, high-saturation
// neon green/amber on every health bar drowns out the alerts that actually need
// attention. Must stay in step with Theme.qml.
inline const QColor kOk(46, 158, 104);
inline const QColor kWarn(184, 137, 58);
inline const QColor kDanger(200, 74, 74);
inline const QColor kOffline(60, 67, 80);

// Numbers use a monospaced face so a changing value does not reflow the row it
// sits in; labels use the UI face because monospaced prose is harder to scan.
QFont numericFont(int point_size, bool bold = false);
QFont labelFont(int point_size, bool bold = false);

// 官方选手端的飘字配色（UI 手册第 08 页）：系统中立信息白、有利信息绿、
// 不利信息红。此前本地实现用蓝/黄/红,与官方语义不符。
// level: 0 信息 1 提示 2 警告 3 严重。
QColor eventColor(std::uint32_t level);

// 斜切平行四边形 —— 官方界面的核心形状语言。cut 为斜切的水平投影像素数,
// 正值切左上/右下,负值反向。用 QPainterPath 而非 QRect 是因为整套官方面板
// (计分板、状态卡、模块灯条) 都靠这个斜角取得一致的视觉节奏。
QPainterPath skewPath(const QRectF& box, qreal cut);

// 单侧斜切：只切一端,用于紧贴屏幕边缘的面板,避免两头都缺角。
QPainterPath skewPathLeading(const QRectF& box, qreal cut, bool mirrored);

// Health drives hue: green above 60%, amber above 25%, red below. An operator
// reads colour before digits, so the colour has to carry the same warning.
QColor healthColor(double ratio);

// Whole-application stylesheet: window, cards, scrollbars, tooltips.
QString styleSheet();

}  // namespace rm_terminal::theme
