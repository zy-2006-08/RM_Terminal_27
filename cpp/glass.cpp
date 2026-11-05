#include "glass.h"

#include <QEnterEvent>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace rm_terminal {

namespace {

QColor with_alpha(QColor colour, int alpha) {
    colour.setAlpha(alpha);
    return colour;
}

QPixmap tinted(const QString& path, const QColor& colour, const QSize& size, qreal dpr) {
    QPixmap source(path);
    if (source.isNull()) return {};
    QPixmap fitted = source.scaled(size * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap out(fitted.size());
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.drawPixmap(0, 0, fitted);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(out.rect(), colour);
    painter.end();
    out.setDevicePixelRatio(dpr);
    return out;
}

}  // namespace

GlassCard::GlassCard(const QString& title, QWidget* parent) : QFrame(parent), title_(title) {
    header_right_ = new QLabel(this);
    header_right_->setObjectName(QStringLiteral("cardHeaderRight"));
    header_right_->setTextFormat(Qt::RichText);
    header_right_->setFont(theme::labelPx(13, QFont::Medium));
    header_right_->setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(theme::kTextSecondary)));
    header_right_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header_right_->setAttribute(Qt::WA_TransparentForMouseEvents);
    header_right_->hide();
    body_ = new QVBoxLayout(this);
    // 卡片的大小由主界面的格子决定,里面放不下就裁掉,不能反过来把卡片撑大压到旁边的面板上。
    body_->setSizeConstraint(QLayout::SetNoConstraint);
    body_->setSpacing(6);
    body_->setContentsMargins(8, title_.isEmpty() ? 8 : kHeaderHeight + 8, 8, 8);
}

void GlassCard::setTitle(const QString& title) {
    if (title_ == title) return;
    title_ = title;
    const QMargins margins = body_->contentsMargins();
    body_->setContentsMargins(margins.left(), title_.isEmpty() ? 8 : kHeaderHeight + 8, margins.right(),
                              margins.bottom());
    update();
}

void GlassCard::setAccent(const QColor& accent) {
    if (accent_ == accent) return;
    accent_ = accent;
    update();
}

void GlassCard::setFill(const QColor& fill) {
    if (fill_ == fill) return;
    fill_ = fill;
    update();
}

void GlassCard::setMirrored(bool mirrored) {
    if (mirrored_ == mirrored) return;
    mirrored_ = mirrored;
    update();
}

void GlassCard::setHeaderRight(const QString& rich_text) {
    if (header_right_->text() == rich_text && header_right_->isVisible() == !rich_text.isEmpty()) return;
    header_right_->setText(rich_text);
    header_right_->setVisible(!rich_text.isEmpty());
    placeHeaderRight();
}

QString GlassCard::headerRight() const { return header_right_->text(); }

void GlassCard::setHeaderInset(int pixels) {
    if (header_inset_ == pixels) return;
    header_inset_ = pixels;
    placeHeaderRight();
}

void GlassCard::placeHeaderRight() {
    const int width =
        std::min(header_right_->sizeHint().width(), std::max(0, this->width() / 2 + 40 - header_inset_));
    header_right_->setGeometry(this->width() - 14 - header_inset_ - width, 0, width, kHeaderHeight);
}

void GlassCard::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    placeHeaderRight();
}

void GlassCard::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    theme::paintGlass(painter, QRectF(rect()), 10, mirrored_, fill_, theme::kBorder, true);
    if (title_.isEmpty()) return;

    painter.setPen(Qt::NoPen);
    painter.setBrush(accent_);
    painter.drawPath(theme::skewBar(QRectF(14, 12.5, 3, 15), -14));

    QFont title = theme::labelPx(15, QFont::Bold);
    title.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    painter.setFont(title);
    painter.setPen(theme::kTextPrimary);
    painter.drawText(QRectF(26, 0, width() - 40, kHeaderHeight), Qt::AlignLeft | Qt::AlignVCenter, title_);

    QLinearGradient rule(12, 0, width() - 12, 0);
    rule.setColorAt(0, theme::kBorderBright);
    rule.setColorAt(0.85, with_alpha(theme::kBorderBright, 0));
    rule.setColorAt(1, with_alpha(theme::kBorderBright, 0));
    painter.fillRect(QRectF(12, kHeaderHeight - 1, width() - 24, 1), rule);
}

GlassButton::GlassButton(const QString& text, QWidget* parent, Kind kind) : QPushButton(text, parent), kind_(kind) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    setFont(labelFont());
}

void GlassButton::setKind(Kind kind) {
    if (kind_ == kind) return;
    kind_ = kind;
    setFont(labelFont());
    updateGeometry();
    update();
}

void GlassButton::setProgress(double progress) {
    progress = std::clamp(progress, 0.0, 1.0);
    if (std::abs(progress_ - progress) < 1e-4) return;
    progress_ = progress;
    update();
}

void GlassButton::setDot(const QColor& colour) {
    if (dot_ == colour) return;
    dot_ = colour;
    updateGeometry();
    update();
}

void GlassButton::setDimTail(bool dim) {
    if (dim_tail_ == dim) return;
    dim_tail_ = dim;
    update();
}

void GlassButton::setBadge(const QString& badge) {
    if (badge_ == badge) return;
    badge_ = badge;
    updateGeometry();
    update();
}

void GlassButton::setGlow(bool glow) {
    if (glow_ == glow) return;
    glow_ = glow;
    update();
}

void GlassButton::setSquare(bool square) {
    if (square_ == square) return;
    square_ = square;
    updateGeometry();
    update();
}

void GlassButton::setTextColour(const QColor& colour) {
    if (text_colour_ == colour) return;
    text_colour_ = colour;
    update();
}

QFont GlassButton::labelFont() const {
    switch (kind_) {
    case Kind::Chip: return theme::labelPx(13, QFont::Medium);
    case Kind::Accent: return theme::labelPx(14, QFont::Bold);
    case Kind::Command: break;
    }
    return theme::labelPx(14, QFont::Medium);
}

QFont GlassButton::numberFont() const {
    if (square_) return theme::numericPx(20, QFont::Medium);
    if (kind_ == Kind::Chip) return theme::numericPx(14, QFont::DemiBold);
    return theme::numericPx(16, QFont::DemiBold);
}

QSize GlassButton::sizeHint() const {
    const int height = kind_ == Kind::Chip ? 28 : 34;
    if (square_) return QSize(42, height);
    const int padding = kind_ == Kind::Chip ? 11 : 16;
    qreal width = theme::mixedTextWidth(text(), labelFont(), numberFont()) + padding * 2;
    if (dot_.alpha() > 0) width += 13;
    if (!badge_.isEmpty()) width += 26;
    return QSize(static_cast<int>(std::ceil(width)), height);
}

QSize GlassButton::minimumSizeHint() const {
    if (kind_ == Kind::Chip || square_) return sizeHint();
    return QSize(48, sizeHint().height());
}

void GlassButton::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QPushButton::enterEvent(event);
}

void GlassButton::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QPushButton::leaveEvent(event);
}

void GlassButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool enabled = isEnabled();
    const bool pressed = isDown() || isChecked();
    QColor fill = theme::kSurfaceRaised;
    QColor stroke = theme::kBorder;
    QColor ink = theme::kTextPrimary;
    switch (kind_) {
    case Kind::Command:
        if (pressed) fill = QColor(30, 60, 92, 158);
        else if (hovered_ && enabled) fill = QColor(52, 76, 116, 122);
        if (isChecked()) {
            stroke = theme::kCyan;
            ink = theme::kAccentText;
        }
        break;
    case Kind::Chip:
        fill = hovered_ && enabled ? QColor(26, 40, 64, 190) : theme::kChip;
        stroke = theme::kRowBorder;
        break;
    case Kind::Accent:
        fill = isDown() ? QColor(30, 70, 104, 230) : theme::kAccentBg;
        stroke = hovered_ && enabled ? theme::kCyan : theme::kCyanDim;
        ink = theme::kAccentText;
        break;
    }
    if (!enabled) {
        fill = QColor(255, 255, 255, 5);
        stroke = theme::kRowBorder;
        ink = theme::kTextMuted;
    }
    if (text_colour_.alpha() > 0 && enabled) ink = text_colour_;

    const QRectF box(rect());
    if (glow_ && enabled) {
        const QPainterPath halo = theme::chamferPath(box.adjusted(0.5, 0.5, -0.5, -0.5), 4);
        for (const auto& [width, alpha] : {std::pair{5.0, 30}, std::pair{3.0, 55}}) {
            painter.setPen(QPen(with_alpha(theme::kCyan, alpha), width));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(halo);
        }
        stroke = theme::kCyan;
    }
    theme::paintGlass(painter, box, 4, false, fill, stroke);
    if (progress_ > 0.0 && enabled) {
        painter.save();
        painter.setClipPath(theme::chamferPath(box.adjusted(1, 1, -1, -1), 4));
        painter.fillRect(QRectF(1, 1, (box.width() - 2) * progress_, box.height() - 2), QColor(84, 156, 204, 120));
        painter.restore();
    }

    QRectF content = box.adjusted(kind_ == Kind::Chip ? 11 : 10, 0, kind_ == Kind::Chip ? -11 : -10, 0);
    if (!badge_.isEmpty()) {
        const QRectF badge(box.right() - 8 - 20, box.center().y() - 9, 20, 18);
        theme::paintGlass(painter, badge, 4, false, QColor(255, 255, 255, 36), Qt::transparent);
        QFont font = theme::numericPx(13, QFont::Bold);
        font.setItalic(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, badge_);
        content.setRight(badge.left() - 4);
    }

    const QFont label = labelFont();
    const QFont number = numberFont();
    QString head = text();
    QString tail;
    if (dim_tail_) {
        int cut = head.indexOf(QStringLiteral(" · "));
        if (cut < 0) cut = head.lastIndexOf(QLatin1Char(' '));
        if (cut > 0) {
            tail = head.mid(cut);
            head = head.left(cut);
        }
    }
    const qreal dot_width = dot_.alpha() > 0 ? 13.0 : 0.0;
    const qreal text_width = theme::mixedTextWidth(head, label, number) + theme::mixedTextWidth(tail, label, number);
    qreal x = content.center().x() - (text_width + dot_width) / 2.0;
    if (dot_.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(enabled ? dot_ : theme::kOffline);
        painter.drawEllipse(QPointF(x + 3.5, content.center().y()), 3.5, 3.5);
        x += dot_width;
    }
    if (square_) {
        painter.setFont(number);
        painter.setPen(enabled ? theme::kTextBright : theme::kTextMuted);
        painter.drawText(box, Qt::AlignCenter, text());
        return;
    }
    const qreal head_width =
        theme::paintMixedText(painter, QRectF(x, content.top(), content.right() - x, content.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, head, label, number, ink);
    if (!tail.isEmpty())
        theme::paintMixedText(painter, QRectF(x + head_width, content.top(), content.width(), content.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, tail, label, number,
                              enabled ? theme::kTextSecondary : theme::kTextMuted);
}

ChipLabel::ChipLabel(QWidget* parent) : QLabel(parent) {
    setFont(theme::labelPx(13, QFont::Medium));
    setAlignment(Qt::AlignCenter);
    setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(theme::kTextPrimary)));
    applyMargins();
}

void ChipLabel::setTone(Tone tone) {
    if (tone_ == tone) return;
    tone_ = tone;
    setStyleSheet(QStringLiteral("color:%1;")
                      .arg(theme::css(tone == Tone::Alert ? theme::kDangerText : theme::kTextPrimary)));
    update();
}

void ChipLabel::setTick(const QColor& colour) {
    if (tick_ == colour) return;
    tick_ = colour;
    applyMargins();
    update();
}

void ChipLabel::applyMargins() { setContentsMargins(tick_.alpha() > 0 ? 21 : 11, 0, 11, 0); }

QSize ChipLabel::sizeHint() const { return QSize(QLabel::sizeHint().width(), 28); }

QSize ChipLabel::minimumSizeHint() const { return QSize(QLabel::minimumSizeHint().width(), 28); }

void ChipLabel::paintEvent(QPaintEvent* event) {
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const bool alert = tone_ == Tone::Alert;
        theme::paintGlass(painter, QRectF(rect()), 4, false, alert ? QColor(70, 14, 22, 200) : theme::kChip,
                          alert ? QColor(255, 100, 100, 120) : theme::kRowBorder);
        if (tick_.alpha() > 0) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(tick_);
            painter.drawPath(theme::skewBar(QRectF(11, height() / 2.0 - 7, 4, 14), -14));
        }
    }
    QLabel::paintEvent(event);
}

AspectFrame::AspectFrame(QWidget* child, double aspect, QWidget* parent)
    : QWidget(parent), child_(child), aspect_(aspect) {
    child_->setParent(this);
}

QSize AspectFrame::sizeHint() const {
    const QSize hint = child_->sizeHint();
    return hint.isValid() ? hint : QSize(400, static_cast<int>(400 / aspect_));
}

void AspectFrame::setFooter(QWidget* footer) {
    footer_ = footer;
    footer_->setParent(this);
    updateGeometry();
}

QSize AspectFrame::minimumSizeHint() const {
    const QSize footer = footer_ ? footer_->minimumSizeHint() : QSize(0, 0);
    return QSize(std::max(160, footer.width()),
                 static_cast<int>(160 / aspect_) + (footer_ ? footer_->sizeHint().height() + 6 : 0));
}

void AspectFrame::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int footer_h = footer_ ? footer_->sizeHint().height() + 6 : 0;
    const int room = std::max(1, height() - footer_h);
    int w = width();
    int h = static_cast<int>(std::lround(w / aspect_));
    if (h > room) {
        h = room;
        w = static_cast<int>(std::lround(h * aspect_));
    }
    const int top = (height() - h - footer_h) / 2;
    child_->setGeometry((width() - w) / 2, top, w, h);
    if (footer_) {
        const int footer_w = std::max(w, std::min(width(), footer_->minimumSizeHint().width()));
        footer_->setGeometry((width() - footer_w) / 2, top + h + 6, footer_w, footer_h - 6);
    }
}

GlowBanner::GlowBanner(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setVisible(false);
}

void GlowBanner::show(const QString& text, Tone tone, double remaining) {
    const bool changed = text != text_ || tone != tone_;
    text_ = text;
    tone_ = tone;
    remaining_ = std::clamp(remaining, 0.0, 1.0);
    if (changed) {
        icon_ = text.contains(QStringLiteral("能量机关"))
                    ? tinted(QStringLiteral(":/images/message/validate_icon_rune.png"), theme::kAccentText,
                             QSize(30, 34), devicePixelRatioF())
                    : QPixmap();
    }
    update();
}

void GlowBanner::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = width();
    const qreal h = height();
    const QColor glow = tone_ == Tone::Info      ? QColor(70, 180, 235)
                        : tone_ == Tone::Warning ? QColor(236, 170, 70)
                                                 : QColor(255, 88, 72);
    const QColor line = tone_ == Tone::Info ? theme::kCyan : tone_ == Tone::Warning ? theme::kWarn : theme::kDanger;

    QLinearGradient band(0, 0, w, 0);
    band.setColorAt(0, QColor(6, 16, 30, 0));
    band.setColorAt(0.12, QColor(6, 16, 30, 235));
    band.setColorAt(0.88, QColor(6, 16, 30, 235));
    band.setColorAt(1, QColor(6, 16, 30, 0));
    painter.fillRect(rect(), band);
    QRadialGradient halo(QPointF(0, 0), 1.0);
    halo.setColorAt(0, with_alpha(glow, 115));
    halo.setColorAt(0.78, with_alpha(glow, 0));
    halo.setColorAt(1, with_alpha(glow, 0));
    QBrush brush(halo);
    QTransform transform;
    transform.translate(w / 2, h / 2);
    transform.scale(w * 0.48, h * 1.5);
    brush.setTransform(transform);
    painter.fillRect(rect(), brush);
    for (const qreal y : {0.0, h - 1}) {
        QLinearGradient edge(w * 0.08, 0, w * 0.92, 0);
        edge.setColorAt(0, with_alpha(line, 0));
        edge.setColorAt(0.5, line);
        edge.setColorAt(1, with_alpha(line, 0));
        painter.fillRect(QRectF(w * 0.08, y, w * 0.84, 1), edge);
    }

    QFont font = theme::labelPx(h >= 60 ? 30 : 22, QFont::Bold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    const QFontMetricsF metrics(font);
    const qreal text_width = std::min(metrics.horizontalAdvance(text_), w - 80);
    const qreal icon_width = icon_.isNull() ? 0 : icon_.width() / icon_.devicePixelRatio() + 16;
    qreal x = (w - text_width - icon_width) / 2.0;
    if (!icon_.isNull()) {
        const QSizeF icon_size = icon_.deviceIndependentSize();
        painter.drawPixmap(QPointF(x, (h - icon_size.height()) / 2.0), icon_);
        x += icon_width;
    }
    const QString shown = metrics.elidedText(text_, Qt::ElideRight, w - 80);
    QPainterPath path;
    path.addText(x, h / 2.0 + (metrics.ascent() - metrics.descent()) / 2.0 - 2, font, shown);
    painter.setBrush(Qt::NoBrush);
    for (const auto& [width, alpha] : {std::pair{10.0, 26}, std::pair{6.0, 40}, std::pair{3.0, 60}}) {
        painter.setPen(QPen(with_alpha(QColor(120, 200, 245), tone_ == Tone::Info ? alpha : alpha / 2), width,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
    }
    painter.setPen(Qt::NoPen);
    painter.fillPath(path.translated(0, 1), QColor(0, 0, 0, 150));
    painter.fillPath(path, Qt::white);

    const QRectF track(w * 0.34, h - 9, w * 0.32, 2);
    painter.fillRect(track, QColor(255, 255, 255, 41));
    painter.fillRect(QRectF(track.left(), track.top(), track.width() * remaining_, track.height()), line);
}

void paintLayoutThumbnail(QPainter& painter, const QRectF& box, const std::vector<PaneSlot>& cells,
                          std::optional<PaneId> highlight) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    theme::paintGlass(painter, box, 4, false, QColor(0, 0, 0, 89), Qt::transparent);
    const QRectF inner = box.adjusted(4, 4, -4, -4);
    const qreal cw = inner.width() / kGridColumns;
    const qreal ch = inner.height() / kGridRows;
    for (const PaneSlot& slot : cells) {
        const QRectF part(inner.left() + slot.column * cw + 1, inner.top() + slot.row * ch + 1, slot.column_span * cw - 2,
                          slot.row_span * ch - 2);
        const bool hot = highlight && slot.pane == *highlight;
        painter.fillRect(part, hot ? theme::kCyan : QColor(150, 180, 230, 77));
    }
    painter.restore();
}

}  // namespace rm_terminal
